/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "icelocaltransport.h"

#include "objectsession.h"
#include "stunallocate.h"
#include "stunbinding.h"
#include "stunmessage.h"
#include "stuntransaction.h"
#include "turnclient.h"

#include <QHostAddress>
#include <QUdpSocket>
#include <QtCrypto>

// don't queue more incoming packets than this per transmit path
#define MAX_PACKET_QUEUE 64

namespace XMPP {
enum { Direct, Relayed };

//----------------------------------------------------------------------------
// SafeUdpSocket
//----------------------------------------------------------------------------
// DOR-safe wrapper for QUdpSocket
class SafeUdpSocket : public QObject {
    Q_OBJECT

private:
    ObjectSession sess;
    QUdpSocket *  sock;
    int           writtenCount;

public:
    SafeUdpSocket(QUdpSocket *_sock, QObject *parent = nullptr) : QObject(parent), sess(this), sock(_sock)
    {
        sock->setParent(this);
        connect(sock, &QUdpSocket::readyRead, this, &SafeUdpSocket::sock_readyRead);
        connect(sock, &QUdpSocket::bytesWritten, this, &SafeUdpSocket::sock_bytesWritten);

        writtenCount = 0;
    }

    ~SafeUdpSocket()
    {
        if (sock) {
            QUdpSocket *out = release();
            out->deleteLater();
        }
    }

    QUdpSocket *release()
    {
        sock->disconnect(this);
        sock->setParent(nullptr);
        QUdpSocket *out = sock;
        sock            = nullptr;
        return out;
    }

    TransportAddress localTransportAddress() const { return { sock->localAddress(), sock->localPort() }; }

    QHostAddress localAddress() const { return sock->localAddress(); }

    quint16 localPort() const { return sock->localPort(); }

    bool hasPendingDatagrams() const { return sock->hasPendingDatagrams(); }

    QByteArray readDatagram(TransportAddress &address)
    {
        if (!sock->hasPendingDatagrams())
            return QByteArray();

        QByteArray buf;
        buf.resize(int(sock->pendingDatagramSize()));
        sock->readDatagram(buf.data(), buf.size(), &address.addr, &address.port);
        return buf;
    }

    void writeDatagram(const QByteArray &buf, const TransportAddress &address)
    {
        sock->writeDatagram(buf, address.addr, address.port);
    }

signals:
    void readyRead();
    void datagramsWritten(int count);

private slots:
    void sock_readyRead() { emit readyRead(); }

    void sock_bytesWritten(qint64 bytes)
    {
        Q_UNUSED(bytes);

        ++writtenCount;
        sess.deferExclusive(this, "processWritten");
    }

    void processWritten()
    {
        int count    = writtenCount;
        writtenCount = 0;

        emit datagramsWritten(count);
    }
};

//----------------------------------------------------------------------------
// IceLocalTransport
//----------------------------------------------------------------------------
class IceLocalTransport::Private : public QObject {
    Q_OBJECT

public:
    class WriteItem {
    public:
        enum Type { Direct, Pool, Turn };

        Type             type;
        TransportAddress addr;
    };

    class Written {
    public:
        TransportAddress addr;
        int              count;
    };

    class Datagram {
    public:
        TransportAddress addr;
        QByteArray       buf;
    };

    IceLocalTransport *      q;
    ObjectSession            sess;
    QUdpSocket *             extSock = nullptr;
    SafeUdpSocket *          sock    = nullptr;
    StunTransactionPool::Ptr pool;
    StunBinding *            stunBinding   = nullptr;
    TurnClient *             turn          = nullptr;
    bool                     turnActivated = false;
    TransportAddress         addr;
    TransportAddress         refAddr;
    TransportAddress         relAddr;
    QHostAddress             refAddrSource;
    TransportAddress         stunBindAddr;
    TransportAddress         stunRelayAddr;
    QString                  stunUser;
    QCA::SecureArray         stunPass;
    QString                  clientSoftware;
    QList<Datagram>          in;
    QList<Datagram>          inRelayed;
    QList<WriteItem>         pendingWrites;
    int                      retryCount = 0;
    bool                     stopping   = false;
    int                      debugLevel = IceTransport::DL_None;

    Private(IceLocalTransport *_q) : QObject(_q), q(_q), sess(this) { }

    ~Private() { reset(); }

    void reset()
    {
        sess.reset();

        delete stunBinding;
        stunBinding = nullptr;

        delete turn;
        turn          = nullptr;
        turnActivated = false;

        if (sock) { // if started
            if (extSock) {
                sock->release(); // detaches the socket but doesn't destroy
                extSock = nullptr;
            }

            delete sock;
            sock = nullptr;
        }

        addr          = TransportAddress();
        relAddr       = TransportAddress();
        refAddr       = TransportAddress();
        refAddrSource = QHostAddress();

        in.clear();
        inRelayed.clear();
        pendingWrites.clear();

        retryCount = 0;
        stopping   = false;
    }

    void start()
    {
        Q_ASSERT(!sock);

        sess.defer(this, "postStart");
    }

    void stop()
    {
        Q_ASSERT(sock);
        if (stopping) {
            emit q->debugLine(QString("local transport %1 is already stopping. just wait...").arg(addr));
            return;
        } else {
            emit q->debugLine(QString("stopping local transport %1.").arg(addr));
        }

        stopping = true;

        if (turn)
            turn->close(); // will emit stopped() eventually calling postStop()
        else
            sess.defer(this, "postStop");
    }

    void stunStart()
    {
        Q_ASSERT(!pool);

        pool = StunTransactionPool::Ptr::create(StunTransaction::Udp);
        pool->setDebugLevel((StunTransactionPool::DebugLevel)debugLevel);
        connect(pool.data(), &StunTransactionPool::outgoingMessage, this, &Private::pool_outgoingMessage);
        connect(pool.data(), &StunTransactionPool::needAuthParams, this, &Private::pool_needAuthParams);
        connect(pool.data(), &StunTransactionPool::debugLine, this, &Private::pool_debugLine);

        pool->setLongTermAuthEnabled(true);
        if (!stunUser.isEmpty()) {
            pool->setUsername(stunUser);
            pool->setPassword(stunPass);
        }

        do_stun();
        do_turn();
    }

    void do_stun()
    {
        if (!stunBindAddr.isValid()) {
            return;
        }
        stunBinding = new StunBinding(pool.data());
        connect(stunBinding, &StunBinding::success, this, [&]() {
            refAddr       = stunBinding->reflexiveAddress();
            refAddrSource = stunBindAddr.addr;

            delete stunBinding;
            stunBinding = nullptr;

            emit q->addressesChanged();
        });
        connect(stunBinding, &StunBinding::error, this, [&](XMPP::StunBinding::Error) {
            delete stunBinding;
            stunBinding = nullptr;
            emit q->error(IceLocalTransport::ErrorStun);
        });
        stunBinding->start(stunBindAddr);
    }

    void do_turn()
    {
        if (!stunRelayAddr.isValid()) {
            return;
        }
        turn = new TurnClient(this);
        turn->setDebugLevel((TurnClient::DebugLevel)debugLevel);
        connect(turn, &TurnClient::connected, this, &Private::turn_connected);
        connect(turn, &TurnClient::tlsHandshaken, this, &Private::turn_tlsHandshaken);
        connect(turn, &TurnClient::closed, this, &Private::turn_closed);
        connect(turn, &TurnClient::activated, this, &Private::turn_activated);
        connect(turn, &TurnClient::packetsWritten, this, &Private::turn_packetsWritten);
        connect(turn, &TurnClient::error, this, &Private::turn_error);
        connect(turn, &TurnClient::outgoingDatagram, this, &Private::turn_outgoingDatagram);
        connect(turn, &TurnClient::debugLine, this, &Private::turn_debugLine);

        turn->setClientSoftwareNameAndVersion(clientSoftware);

        turn->connectToHost(pool.data(), stunRelayAddr);
    }

private:
    // note: emits signal on error
    QUdpSocket *createSocket()
    {
        QUdpSocket *qsock = new QUdpSocket(this);
        if (!qsock->bind(addr.addr, 0)) {
            delete qsock;
            emit q->error(IceLocalTransport::ErrorBind);
            return nullptr;
        }

        return qsock;
    }

    void prepareSocket()
    {
        addr = sock->localTransportAddress();

        connect(sock, &SafeUdpSocket::readyRead, this, &Private::sock_readyRead);
        connect(sock, &SafeUdpSocket::datagramsWritten, this, &Private::sock_datagramsWritten);
    }

    // return true if we are retrying, false if we should error out
    bool handleRetry()
    {
        // don't allow retrying if activated or stopping)
        if (turnActivated || stopping)
            return false;

        ++retryCount;
        if (retryCount < 3) {
            if (debugLevel >= IceTransport::DL_Info)
                emit q->debugLine("retrying...");

            delete sock;
            sock = nullptr;

            // to receive this error, it is a Relay, so change
            //   the mode
            // stunType = IceLocalTransport::Relay;

            QUdpSocket *qsock = createSocket();
            if (!qsock) {
                // signal emitted in this case.  bail.
                //   (return true so caller takes no action)
                return true;
            }

            sock = new SafeUdpSocket(qsock, this);

            prepareSocket();

            refAddr       = TransportAddress();
            refAddrSource = QHostAddress();

            relAddr = TransportAddress();

            do_turn();

            // tell the world that our local address probably
            //   changed, and that we lost our reflexive address
            emit q->addressesChanged();
            return true;
        }

        return false;
    }

    // return true if data packet, false if pool or nothing
    bool processIncomingStun(const QByteArray &buf, const TransportAddress &fromAddr, Datagram *dg)
    {
        QByteArray       data;
        TransportAddress dataAddr;

        bool notStun;
        if (!pool->writeIncomingMessage(buf, &notStun, fromAddr) && turn) {
            data = turn->processIncomingDatagram(buf, notStun, dataAddr);
            if (!data.isNull()) {
                dg->addr = dataAddr;
                dg->buf  = data;
                return true;
            } else {
                if (debugLevel >= IceTransport::DL_Packet)
                    emit q->debugLine(
                        "Warning: server responded with what doesn't seem to be a STUN or data packet, skipping.");
            }
        }

        return false;
    }

private slots:
    void postStart()
    {
        if (stopping)
            return;

        if (extSock) {
            sock = new SafeUdpSocket(extSock, this);
        } else {
            QUdpSocket *qsock = createSocket();
            if (!qsock) {
                // signal emitted in this case.  bail
                return;
            }

            sock = new SafeUdpSocket(qsock, this);
        }

        prepareSocket();

        emit q->started();
    }

    void postStop()
    {
        reset();
        emit q->stopped();
    }

    void sock_readyRead()
    {
        ObjectSessionWatcher watch(&sess);

        QList<Datagram> dreads; // direct
        QList<Datagram> rreads; // relayed

        while (sock->hasPendingDatagrams()) {
            TransportAddress from;
            Datagram         dg;

            QByteArray buf = sock->readDatagram(from);
            if (buf.isEmpty()) // it's weird we ever came here, but should relax static analyzer
                break;
            qDebug("got packet from %s", qPrintable(from));
            if (from == stunBindAddr || from == stunRelayAddr) {
                bool haveData = processIncomingStun(buf, from, &dg);

                // processIncomingStun could cause signals to
                //   emit.  for example, stopped()
                if (!watch.isValid())
                    return;

                if (haveData)
                    rreads += dg;
            } else {
                dg.addr = from;
                dg.buf  = buf;
                dreads += dg;
            }
        }

        if (dreads.count() > 0) {
            in += dreads;
            emit q->readyRead(Direct);
            if (!watch.isValid())
                return;
        }

        if (rreads.count() > 0) {
            inRelayed += rreads;
            emit q->readyRead(Relayed);
        }
    }

    void sock_datagramsWritten(int count)
    {
        QList<Written> dwrites;
        int            twrites = 0;

        while (count > 0) {
            Q_ASSERT(!pendingWrites.isEmpty());
            WriteItem wi = pendingWrites.takeFirst();
            --count;

            if (wi.type == WriteItem::Direct) {
                int at = -1;
                for (int n = 0; n < dwrites.count(); ++n) {
                    if (dwrites[n].addr == wi.addr) {
                        at = n;
                        break;
                    }
                }

                if (at != -1) {
                    ++dwrites[at].count;
                } else {
                    Written wr;
                    wr.addr  = wi.addr;
                    wr.count = 1;
                    dwrites += wr;
                }
            } else if (wi.type == WriteItem::Turn)
                ++twrites;
        }

        if (dwrites.isEmpty() && twrites == 0)
            return;

        ObjectSessionWatcher watch(&sess);

        if (!dwrites.isEmpty()) {
            for (const Written &wr : qAsConst(dwrites)) {
                emit q->datagramsWritten(Direct, wr.count, wr.addr);
                if (!watch.isValid())
                    return;
            }
        }

        if (twrites > 0) {
            // note: this will invoke turn_packetsWritten()
            turn->outgoingDatagramsWritten(twrites);
        }
    }

    void pool_outgoingMessage(const QByteArray &packet, const XMPP::TransportAddress &toAddress)
    {
        // warning: read StunTransactionPool docs before modifying
        //   this function

        WriteItem wi;
        wi.type = WriteItem::Pool;
        pendingWrites += wi;
        // emit q->debugLine(QString("Sending udp packet from: %1:%2 to: %3:%4")
        //                      .arg(sock->localAddress().toString())
        //                      .arg(sock->localPort())
        //                      .arg(toAddress.toString())
        //                      .arg(toPort));

        sock->writeDatagram(packet, toAddress);
    }

    void pool_needAuthParams(const TransportAddress &addr)
    {
        // we can get this signal if the user did not provide
        //   creds to us.  however, since this class doesn't support
        //   prompting just continue on as if we had a blank
        //   user/pass
        pool->continueAfterParams(addr);
    }

    void pool_debugLine(const QString &line) { emit q->debugLine(line); }

    void turn_connected()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_connected");
    }

    void turn_tlsHandshaken()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_tlsHandshaken");
    }

    void turn_closed()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_closed");

        delete turn;
        turn          = nullptr;
        turnActivated = false;

        postStop();
    }

    void turn_activated()
    {
        StunAllocate *allocate = turn->stunAllocate();

        // take reflexive address from TURN only if we are not using a
        //   separate STUN server
        if (!stunBindAddr.isValid() || stunBindAddr == stunRelayAddr) {
            refAddr       = allocate->reflexiveAddress();
            refAddrSource = stunRelayAddr.addr;
        }

        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QLatin1String("Server says we are ") + allocate->reflexiveAddress());

        relAddr = allocate->relayedAddress();
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QLatin1String("Server relays via ") + relAddr);

        turnActivated = true;

        emit q->addressesChanged();
    }

    void turn_packetsWritten(int count, const XMPP::TransportAddress &addr)
    {
        emit q->datagramsWritten(Relayed, count, addr);
    }

    void turn_error(XMPP::TurnClient::Error e)
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QString("turn_error: ") + turn->errorString());

        delete turn;
        turn              = nullptr;
        bool wasActivated = turnActivated;
        turnActivated     = false;

        if (e == TurnClient::ErrorMismatch) {
            if (!extSock && handleRetry())
                return;
        }

        // this means our relay died on us.  in the future we might
        //   consider reporting this
        if (wasActivated)
            return;

        emit q->error(IceLocalTransport::ErrorTurn);
        // don't report any error
        // if(stunType == IceLocalTransport::Relay || (stunType == IceLocalTransport::Auto && !stunBinding))
        //    emit q->addressesChanged();
    }

    void turn_outgoingDatagram(const QByteArray &buf)
    {
        WriteItem wi;
        wi.type = WriteItem::Turn;
        pendingWrites += wi;
        sock->writeDatagram(buf, stunRelayAddr);
    }

    void turn_debugLine(const QString &line) { emit q->debugLine(line); }
};

IceLocalTransport::IceLocalTransport(QObject *parent) : IceTransport(parent) { d = new Private(this); }

IceLocalTransport::~IceLocalTransport() { delete d; }

void IceLocalTransport::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void IceLocalTransport::start(QUdpSocket *sock)
{
    d->extSock = sock;
    d->start();
}

void IceLocalTransport::start(const QHostAddress &addr)
{
    d->addr.addr = addr;
    d->start();
}

void IceLocalTransport::stop() { d->stop(); }

void IceLocalTransport::setStunBindService(const TransportAddress &addr) { d->stunBindAddr = addr; }

void IceLocalTransport::setStunRelayService(const TransportAddress &addr, const QString &user,
                                            const QCA::SecureArray &pass)
{
    d->stunRelayAddr = addr;
    d->stunUser      = user;
    d->stunPass      = pass;
}

const TransportAddress &IceLocalTransport::stunBindServiceAddress() const { return d->stunBindAddr; }

const TransportAddress &IceLocalTransport::stunRelayServiceAddress() const { return d->stunRelayAddr; }

void IceLocalTransport::stunStart() { d->stunStart(); }

const TransportAddress &IceLocalTransport::localAddress() const { return d->addr; }

const TransportAddress &IceLocalTransport::serverReflexiveAddress() const { return d->refAddr; }

QHostAddress IceLocalTransport::reflexiveAddressSource() const { return d->refAddrSource; }

const TransportAddress &IceLocalTransport::relayedAddress() const { return d->relAddr; }

bool IceLocalTransport::isStunAlive() const { return d->stunBinding != nullptr; }

bool IceLocalTransport::isTurnAlive() const { return d->turn != nullptr; }

void IceLocalTransport::addChannelPeer(const TransportAddress &addr)
{
    if (d->turn)
        d->turn->addChannelPeer(addr);
}

bool IceLocalTransport::hasPendingDatagrams(int path) const
{
    if (path == Direct)
        return !d->in.isEmpty();
    else if (path == Relayed)
        return !d->inRelayed.isEmpty();
    else {
        Q_ASSERT(0);
        return false;
    }
}

QByteArray IceLocalTransport::readDatagram(int path, TransportAddress &addr)
{
    QList<Private::Datagram> *in = nullptr;
    if (path == Direct)
        in = &d->in;
    else if (path == Relayed)
        in = &d->inRelayed;
    else
        Q_ASSERT(0);

    if (!in->isEmpty()) {
        Private::Datagram datagram = in->takeFirst();
        addr                       = datagram.addr;
        return datagram.buf;
    } else
        return QByteArray();
}

void IceLocalTransport::writeDatagram(int path, const QByteArray &buf, const TransportAddress &addr)
{
    if (path == Direct) {
        Private::WriteItem wi;
        wi.type = Private::WriteItem::Direct;
        wi.addr = addr;
        d->pendingWrites += wi;
        d->sock->writeDatagram(buf, addr);
    } else if (path == Relayed) {
        if (d->turn && d->turnActivated)
            d->turn->write(buf, addr);
    } else
        Q_ASSERT(0);
}

void IceLocalTransport::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    if (d->pool)
        d->pool->setDebugLevel((StunTransactionPool::DebugLevel)level);
    if (d->turn)
        d->turn->setDebugLevel((TurnClient::DebugLevel)level);
}

void IceLocalTransport::changeThread(QThread *thread)
{
    if (d->pool)
        d->pool->moveToThread(thread);
    moveToThread(thread);
}

} // namespace XMPP

#include "icelocaltransport.moc"
