/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
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

#include "turnclient.h"

#include "bsocket.h"
#include "bytestream.h"
#include "httpconnect.h"
#include "objectsession.h"
#include "socks.h"
#include "stunallocate.h"
#include "stunmessage.h"
#include "stuntransaction.h"
#include "stuntypes.h"

#include <QtCrypto>

namespace XMPP {
//----------------------------------------------------------------------------
// TurnClient::Proxy
//----------------------------------------------------------------------------
TurnClient::Proxy::Proxy() : t(None) { }

TurnClient::Proxy::~Proxy() { }

int TurnClient::Proxy::type() const { return t; }

QString TurnClient::Proxy::host() const { return v_host; }

quint16 TurnClient::Proxy::port() const { return v_port; }

QString TurnClient::Proxy::user() const { return v_user; }

QString TurnClient::Proxy::pass() const { return v_pass; }

void TurnClient::Proxy::setHttpConnect(const QString &host, quint16 port)
{
    t      = HttpConnect;
    v_host = host;
    v_port = port;
}

void TurnClient::Proxy::setSocks(const QString &host, quint16 port)
{
    t      = Socks;
    v_host = host;
    v_port = port;
}

void TurnClient::Proxy::setUserPass(const QString &user, const QString &pass)
{
    v_user = user;
    v_pass = pass;
}

//----------------------------------------------------------------------------
// TurnClient
//----------------------------------------------------------------------------
class TurnClient::Private : public QObject {
    Q_OBJECT

public:
    TurnClient *             q;
    Proxy                    proxy;
    QString                  clientSoftware;
    TurnClient::Mode         mode = PlainMode;
    TransportAddress         serverAddr;
    ObjectSession            sess;
    ByteStream *             bs            = nullptr;
    QCA::TLS *               tls           = nullptr;
    bool                     tlsHandshaken = false;
    QByteArray               inStream;
    bool                     udp = false;
    StunTransactionPool::Ptr pool;
    StunAllocate *           allocate        = nullptr;
    bool                     allocateStarted = false;
    QString                  user;
    QCA::SecureArray         pass;
    QString                  realm;
    int                      retryCount = 0;
    QString                  errorString;
    int                      debugLevel = 0;

    class WriteItem {
    public:
        enum Type { Data, Other };

        Type             type;
        int              size;
        TransportAddress addr;

        WriteItem(int _size) : type(Other), size(_size) { }

        WriteItem(int _size, const TransportAddress &_addr) : type(Data), size(_size), addr(_addr) { }
    };

    QList<WriteItem> writeItems;
    int              writtenBytes;
    bool             stopping;

    class Packet {
    public:
        TransportAddress addr;
        QByteArray       data;

        // for outbound
        bool requireChannel;

        Packet() : requireChannel(false) { }
    };

    QList<Packet>                in;
    QList<Packet>                outPending;
    int                          outPendingWrite;
    QList<QHostAddress>          desiredPerms;
    QList<StunAllocate::Channel> pendingChannels, desiredChannels;

    class Written {
    public:
        TransportAddress addr;
        int              count;
    };

    Private(TurnClient *_q) :
        QObject(_q), q(_q), sess(this), bs(nullptr), tls(0), udp(false), pool(nullptr), allocate(nullptr),
        retryCount(0), debugLevel(TurnClient::DL_None), writtenBytes(0), stopping(false), outPendingWrite(0)
    {
    }

    ~Private() { cleanup(); }

    void unsetPool()
    {
        // in udp mode, we don't own the pool
        if (!udp && pool) {
            pool->disconnect(this);
            pool.reset();
        }
    }

    void cleanup()
    {
        delete allocate;
        allocate = nullptr;

        unsetPool();

        delete tls;
        tls = nullptr;

        delete bs;
        bs = 0;

        udp = false;

        sess.reset();

        inStream.clear();
        retryCount = 0;
        writeItems.clear();
        writtenBytes = 0;
        stopping     = false;
        outPending.clear();
        outPendingWrite = 0;
        desiredPerms.clear();
        pendingChannels.clear();
        desiredChannels.clear();
    }

    void do_connect()
    {
        if (udp) {
            after_connected();
            return;
        }

        if (proxy.type() == Proxy::HttpConnect) {
            HttpConnect *s = new HttpConnect(this);
            bs             = s;
            connect(s, &HttpConnect::connected, this, &Private::bs_connected);
            connect(s, &HttpConnect::error, this, &Private::bs_error);
            if (!proxy.user().isEmpty())
                s->setAuth(proxy.user(), proxy.pass());
            s->connectToHost(proxy.host(), proxy.port(), serverAddr.addr.toString(), serverAddr.port);
        } else if (proxy.type() == Proxy::Socks) {
            SocksClient *s = new SocksClient(this);
            bs             = s;
            connect(s, &SocksClient::connected, this, &Private::bs_connected);
            connect(s, &SocksClient::error, this, &Private::bs_error);
            if (!proxy.user().isEmpty())
                s->setAuth(proxy.user(), proxy.pass());
            s->connectToHost(proxy.host(), proxy.port(), serverAddr.addr.toString(), serverAddr.port);
        } else {
            BSocket *s = new BSocket(this);
            bs         = s;
            connect(s, &BSocket::connected, this, &Private::bs_connected);
            connect(s, &BSocket::error, this, &Private::bs_error);
            s->connectToHost(serverAddr.addr.toString(), serverAddr.port);
        }

        connect(bs, &ByteStream::connectionClosed, this, &Private::bs_connectionClosed);
        connect(bs, &ByteStream::delayedCloseFinished, this, &Private::bs_delayedCloseFinished);
        connect(bs, &ByteStream::readyRead, this, &Private::bs_readyRead);
        connect(bs, &ByteStream::bytesWritten, this, &Private::bs_bytesWritten);
    }

    void do_close()
    {
        stopping = true;

        if (allocate && allocateStarted) {
            if (debugLevel >= TurnClient::DL_Info)
                emit q->debugLine("Deallocating...");
            allocate->stop();
        } else {
            delete allocate;
            allocate = nullptr;

            unsetPool();

            if (udp)
                sess.defer(q, "closed");
            else
                do_transport_close();
        }
    }

    void do_transport_close()
    {
        if (tls && tlsHandshaken) {
            tls->close();
        } else {
            delete tls;
            tls = nullptr;

            do_sock_close();
        }
    }

    void do_sock_close()
    {
        bool waitForSignal = false;
        if (bs->bytesToWrite() > 0)
            waitForSignal = true;

        bs->close();
        if (!waitForSignal) {
            cleanup();
            sess.defer(q, "closed");
        }
    }

    void after_connected()
    {
        // when retrying, pool will be non-null because we reuse it
        if (!udp && !pool) {
            pool = StunTransactionPool::Ptr::create(StunTransaction::Tcp);
            pool->setDebugLevel((StunTransactionPool::DebugLevel)debugLevel);
            connect(pool.data(), &StunTransactionPool::outgoingMessage, this, &Private::pool_outgoingMessage);
            connect(pool.data(), &StunTransactionPool::needAuthParams, this, &Private::pool_needAuthParams);
            connect(pool.data(), &StunTransactionPool::debugLine, this, &Private::pool_debugLine);

            pool->setLongTermAuthEnabled(true);
            if (!user.isEmpty()) {
                pool->setUsername(user);
                pool->setPassword(pass);
                if (!realm.isEmpty())
                    pool->setRealm(realm);
            }
        }

        allocate = new StunAllocate(pool.data());
        connect(allocate, &StunAllocate::started, this, &Private::allocate_started);
        connect(allocate, &StunAllocate::stopped, this, &Private::allocate_stopped);
        connect(allocate, &StunAllocate::error, this, &Private::allocate_error);
        connect(allocate, &StunAllocate::permissionsChanged, this, &Private::allocate_permissionsChanged);
        connect(allocate, &StunAllocate::channelsChanged, this, &Private::allocate_channelsChanged);
        connect(allocate, &StunAllocate::debugLine, this, &Private::allocate_debugLine);

        allocate->setClientSoftwareNameAndVersion(clientSoftware);

        allocateStarted = false;
        if (debugLevel >= TurnClient::DL_Info)
            emit q->debugLine("Allocating...");
        // only use addr association in udp mode
        if (udp)
            allocate->start(serverAddr);
        else
            allocate->start();
    }

    void processStream(const QByteArray &in)
    {
        inStream += in;

        ObjectSessionWatcher watch(&sess);
        while (1) {
            QByteArray packet;

            // try to extract ChannelData or a STUN message from
            //   the stream
            packet = StunAllocate::readChannelData((const quint8 *)inStream.data(), inStream.size());
            if (packet.isNull()) {
                packet = StunMessage::readStun((const quint8 *)inStream.data(), inStream.size());
                if (packet.isNull())
                    break;
            }

            inStream = inStream.mid(packet.size());

            // processDatagram may cause the session to be reset
            //   or the object to be deleted
            processDatagram(packet);
            if (!watch.isValid())
                break;
        }
    }

    void processDatagram(const QByteArray &buf)
    {
        bool notStun;
        if (!pool->writeIncomingMessage(buf, &notStun)) {
            QByteArray       data;
            TransportAddress fromAddr;

            data = processNonPoolPacket(buf, notStun, fromAddr);
            if (!data.isNull())
                processDataPacket(data, fromAddr);
        }
    }

    QByteArray processNonPoolPacket(const QByteArray &buf, bool notStun, TransportAddress &addr)
    {
        if (notStun) {
            // not stun?  maybe it is a data packet
            QByteArray data = allocate->decode(buf, addr);
            if (!data.isNull()) {
                if (debugLevel >= TurnClient::DL_Packet)
                    emit q->debugLine("Received ChannelData-based data packet");
                return data;
            }
        } else {
            // packet might be stun not owned by pool.
            //   let's see
            StunMessage message = StunMessage::fromBinary(buf);
            if (!message.isNull()) {
                QByteArray data = allocate->decode(message, addr);

                if (!data.isNull()) {
                    if (debugLevel >= TurnClient::DL_Packet)
                        emit q->debugLine("Received STUN-based data packet");
                    return data;
                } else {
                    if (debugLevel >= TurnClient::DL_Packet)
                        emit q->debugLine("Warning: server responded with an unexpected STUN packet, skipping.");
                }

                return QByteArray();
            }
        }

        if (debugLevel >= TurnClient::DL_Packet)
            emit q->debugLine(
                "Warning: server responded with what doesn't seem to be a STUN or data packet, skipping.");
        return QByteArray();
    }

    void processDataPacket(const QByteArray &buf, const TransportAddress &addr)
    {
        Packet p;
        p.addr = addr;
        p.data = buf;
        in += p;

        emit q->readyRead();
    }

    void writeOrQueue(const QByteArray &buf, const TransportAddress &addr)
    {
        Q_ASSERT(allocateStarted);

        StunAllocate::Channel c(addr);
        bool                  writeImmediately = false;
        bool                  requireChannel   = pendingChannels.contains(c) || desiredChannels.contains(c);

        QList<QHostAddress> actualPerms = allocate->permissions();
        if (actualPerms.contains(addr.addr)) {
            if (requireChannel) {
                QList<StunAllocate::Channel> actualChannels = allocate->channels();
                if (actualChannels.contains(c))
                    writeImmediately = true;
            } else
                writeImmediately = true;
        }

        if (writeImmediately) {
            write(buf, addr);
        } else {
            Packet p;
            p.addr           = addr;
            p.data           = buf;
            p.requireChannel = requireChannel;
            outPending += p;

            ensurePermission(addr.addr);
        }
    }

    void tryWriteQueued()
    {
        QList<QHostAddress>          actualPerms    = allocate->permissions();
        QList<StunAllocate::Channel> actualChannels = allocate->channels();
        for (int n = 0; n < outPending.count(); ++n) {
            const Packet &p = outPending[n];
            if (actualPerms.contains(p.addr.addr)) {
                StunAllocate::Channel c(p.addr);
                if (!p.requireChannel || actualChannels.contains(c)) {
                    Packet po = outPending[n];
                    outPending.removeAt(n);
                    --n; // adjust position

                    write(po.data, po.addr);
                }
            }
        }
    }

    void tryChannelQueued()
    {
        if (!pendingChannels.isEmpty()) {
            QList<QHostAddress>          actualPerms = allocate->permissions();
            QList<StunAllocate::Channel> list;
            for (int n = 0; n < pendingChannels.count(); ++n) {
                if (actualPerms.contains(pendingChannels[n].address.addr)) {
                    list += pendingChannels[n];
                    pendingChannels.removeAt(n);
                    --n; // adjust position
                }
            }

            if (!list.isEmpty())
                ensureChannels(list);
        }
    }

    void write(const QByteArray &buf, const TransportAddress &addr)
    {
        QByteArray packet = allocate->encode(buf, addr);

        if (debugLevel >= TurnClient::DL_Packet) {
            StunMessage msg = StunMessage::fromBinary(packet);
            if (!msg.isNull()) {
                emit q->debugLine("STUN SEND");
                emit q->debugLine(StunTypes::print_packet_str(msg));
            } else
                emit q->debugLine("Sending ChannelData-based data packet");
        }

        writeItems += WriteItem(packet.size(), addr);
        ++outPendingWrite;
        if (udp) {
            emit q->outgoingDatagram(packet);
        } else {
            if (tls)
                tls->write(packet);
            else
                bs->write(packet);
        }
    }

    void ensurePermission(const QHostAddress &addr)
    {
        if (!desiredPerms.contains(addr)) {
            if (debugLevel >= TurnClient::DL_Info)
                emit q->debugLine(QString("Setting permission for peer address %1").arg(addr.toString()));

            desiredPerms += addr;
            allocate->setPermissions(desiredPerms);
        }
    }

    // assumes we have perms for all input already
    void ensureChannels(const QList<StunAllocate::Channel> &channels)
    {
        bool changed = false;
        for (const StunAllocate::Channel &c : channels) {
            if (!desiredChannels.contains(c)) {
                if (debugLevel >= TurnClient::DL_Info)
                    emit q->debugLine(QString("Setting channel for peer address/port %1").arg(c.address));

                desiredChannels += c;
                changed = true;
            }
        }

        if (changed)
            allocate->setChannels(desiredChannels);
    }

    void addChannelPeer(const TransportAddress &addr)
    {
        ensurePermission(addr.addr);

        StunAllocate::Channel c(addr);
        if (!pendingChannels.contains(c) && !desiredChannels.contains(c)) {
            pendingChannels += c;

            tryChannelQueued();
        }
    }

    void udp_datagramsWritten(int count)
    {
        QList<Written> writtenDests;

        while (count > 0) {
            Q_ASSERT(!writeItems.isEmpty());
            WriteItem wi = writeItems.takeFirst();
            --count;

            if (wi.type == WriteItem::Data) {
                int at = -1;
                for (int n = 0; n < writtenDests.count(); ++n) {
                    if (writtenDests[n].addr == wi.addr) {
                        at = n;
                        break;
                    }
                }

                if (at != -1) {
                    ++writtenDests[at].count;
                } else {
                    Written wr;
                    wr.addr  = wi.addr;
                    wr.count = 1;
                    writtenDests += wr;
                }
            }
        }

        emitPacketsWritten(writtenDests);
    }

    void emitPacketsWritten(const QList<Written> &writtenDests)
    {
        ObjectSessionWatcher watch(&sess);
        for (const Written &wr : writtenDests) {
            emit q->packetsWritten(wr.count, wr.addr);
            if (!watch.isValid())
                return;
        }
    }

    // return true if we are retrying, false if we should error out
    bool handleRetry()
    {
        ++retryCount;
        if (retryCount < 3 && !stopping) {
            if (debugLevel >= TurnClient::DL_Info)
                emit q->debugLine("retrying...");

            // start completely over, but retain the same pool
            //   so the user isn't asked to auth again

            int                      tmp_retryCount = retryCount;
            StunTransactionPool::Ptr tmp_pool       = pool;
            pool.reset();

            cleanup();

            retryCount = tmp_retryCount;
            pool       = tmp_pool;

            do_connect();
            return true;
        }

        return false;
    }

private slots:
    void bs_connected()
    {
        ObjectSessionWatcher watch(&sess);
        emit                 q->connected();
        if (!watch.isValid())
            return;

        if (mode == TurnClient::TlsMode) {
            tls = new QCA::TLS(this);
            connect(tls, &QCA::TLS::handshaken, this, &Private::tls_handshaken);
            connect(tls, &QCA::TLS::readyRead, this, &Private::tls_readyRead);
            connect(tls, &QCA::TLS::readyReadOutgoing, this, &Private::tls_readyReadOutgoing);
            connect(tls, &QCA::TLS::error, this, &Private::tls_error);
            tlsHandshaken = false;
            if (debugLevel >= TurnClient::DL_Info)
                emit q->debugLine("TLS handshaking...");
            tls->startClient();
        } else
            after_connected();
    }

    void bs_connectionClosed()
    {
        cleanup();
        errorString = "Server unexpectedly disconnected.";
        emit q->error(TurnClient::ErrorStream);
    }

    void bs_delayedCloseFinished()
    {
        cleanup();
        emit q->closed();
    }

    void bs_readyRead()
    {
        QByteArray buf = bs->readAll();

        if (tls)
            tls->writeIncoming(buf);
        else
            processStream(buf);
    }

    void bs_bytesWritten(qint64 written)
    {
        if (tls) {
            // convertBytesWritten() is unsafe to call unless
            //   the TLS handshake is completed
            if (!tlsHandshaken)
                return;

            written = tls->convertBytesWritten(written);
        }

        writtenBytes += written;

        QList<Written> writtenDests;

        while (writtenBytes > 0) {
            Q_ASSERT(!writeItems.isEmpty());
            if (writtenBytes < writeItems.first().size)
                break;

            WriteItem wi = writeItems.takeFirst();
            writtenBytes -= wi.size;

            if (wi.type == WriteItem::Data) {
                int at = -1;
                for (int n = 0; n < writtenDests.count(); ++n) {
                    if (writtenDests[n].addr == wi.addr) {
                        at = n;
                        break;
                    }
                }

                if (at != -1) {
                    ++writtenDests[at].count;
                } else {
                    Written wr;
                    wr.addr  = wi.addr;
                    wr.count = 1;
                    writtenDests += wr;
                }
            }
        }

        emitPacketsWritten(writtenDests);
    }

    void bs_error(int e)
    {
        TurnClient::Error te;
        if (qobject_cast<HttpConnect *>(bs)) {
            if (e == HttpConnect::ErrConnectionRefused)
                te = TurnClient::ErrorConnect;
            else if (e == HttpConnect::ErrHostNotFound)
                te = TurnClient::ErrorHostNotFound;
            else if (e == HttpConnect::ErrProxyConnect)
                te = TurnClient::ErrorProxyConnect;
            else if (e == HttpConnect::ErrProxyNeg)
                te = TurnClient::ErrorProxyNeg;
            else if (e == HttpConnect::ErrProxyAuth)
                te = TurnClient::ErrorProxyAuth;
            else
                te = TurnClient::ErrorStream;
        } else if (qobject_cast<SocksClient *>(bs)) {
            if (e == SocksClient::ErrConnectionRefused)
                te = TurnClient::ErrorConnect;
            else if (e == SocksClient::ErrHostNotFound)
                te = TurnClient::ErrorHostNotFound;
            else if (e == SocksClient::ErrProxyConnect)
                te = TurnClient::ErrorProxyConnect;
            else if (e == SocksClient::ErrProxyNeg)
                te = TurnClient::ErrorProxyNeg;
            else if (e == SocksClient::ErrProxyAuth)
                te = TurnClient::ErrorProxyAuth;
            else
                te = TurnClient::ErrorStream;
        } else {
            if (e == BSocket::ErrConnectionRefused)
                te = TurnClient::ErrorConnect;
            else if (e == BSocket::ErrHostNotFound)
                te = TurnClient::ErrorHostNotFound;
            else
                te = TurnClient::ErrorStream;
        }

        cleanup();
        errorString = "Transport error.";
        emit q->error(te);
    }

    void tls_handshaken()
    {
        tlsHandshaken = true;

        ObjectSessionWatcher watch(&sess);
        emit                 q->tlsHandshaken();
        if (!watch.isValid())
            return;

        tls->continueAfterStep();
        after_connected();
    }

    void tls_readyRead() { processStream(tls->read()); }

    void tls_readyReadOutgoing() { bs->write(tls->readOutgoing()); }

    void tls_closed()
    {
        delete tls;
        tls = nullptr;

        do_sock_close();
    }

    void tls_error()
    {
        cleanup();
        errorString = "TLS error.";
        emit q->error(TurnClient::ErrorTls);
    }

    void pool_outgoingMessage(const QByteArray &packet, const XMPP::TransportAddress &toAddress)
    {
        // we aren't using IP-associated transactions
        Q_UNUSED(toAddress);

        writeItems += WriteItem(packet.size());

        // emit q->debugLine(QString("Sending turn packet to: %1:%2")
        //                      .arg(bs->abstractSocket()->peerAddress().toString())
        //                      .arg(bs->abstractSocket()->peerPort()));
        if (tls)
            tls->write(packet);
        else
            bs->write(packet);
    }

    void pool_needAuthParams() { emit q->needAuthParams(); }

    void pool_debugLine(const QString &line) { emit q->debugLine(line); }

    void allocate_started()
    {
        allocateStarted = true;
        if (debugLevel >= TurnClient::DL_Info)
            emit q->debugLine("Allocate started");

        emit q->activated();
    }

    void allocate_stopped()
    {
        delete allocate;
        allocate = nullptr;

        unsetPool();

        if (udp)
            emit q->closed();
        else
            do_transport_close();
    }

    void allocate_error(XMPP::StunAllocate::Error e)
    {
        QString str = allocate->errorString();

        TurnClient::Error te;
        if (e == StunAllocate::ErrorAuth)
            te = TurnClient::ErrorAuth;
        else if (e == StunAllocate::ErrorRejected)
            te = TurnClient::ErrorRejected;
        else if (e == StunAllocate::ErrorProtocol)
            te = TurnClient::ErrorProtocol;
        else if (e == StunAllocate::ErrorCapacity)
            te = TurnClient::ErrorCapacity;
        else if (e == StunAllocate::ErrorMismatch) {
            if (!udp && handleRetry())
                return;

            te = TurnClient::ErrorMismatch;
        } else
            te = TurnClient::ErrorGeneric;

        cleanup();
        errorString = str;
        emit q->error(te);
    }

    void allocate_permissionsChanged()
    {
        if (debugLevel >= TurnClient::DL_Info)
            emit q->debugLine("PermissionsChanged");

        tryChannelQueued();
        tryWriteQueued();
    }

    void allocate_channelsChanged()
    {
        if (debugLevel >= TurnClient::DL_Info)
            emit q->debugLine("ChannelsChanged");

        tryWriteQueued();
    }

    void allocate_debugLine(const QString &line) { emit q->debugLine(line); }
};

TurnClient::TurnClient(QObject *parent) : QObject(parent) { d = new Private(this); }

TurnClient::~TurnClient() { delete d; }

void TurnClient::setProxy(const Proxy &proxy) { d->proxy = proxy; }

void TurnClient::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void TurnClient::connectToHost(StunTransactionPool *pool, const TransportAddress &addr)
{
    d->serverAddr = addr;
    d->udp        = true;
    d->pool       = pool->sharedFromThis();
    d->in.clear();
    d->do_connect();
}

void TurnClient::connectToHost(const TransportAddress &addr, Mode mode)
{
    d->serverAddr = addr;
    d->udp        = false;
    d->mode       = mode;
    d->in.clear();
    d->do_connect();
}

const TransportAddress &TurnClient::serverAddress() const { return d->serverAddr; }

QByteArray TurnClient::processIncomingDatagram(const QByteArray &buf, bool notStun, TransportAddress &addr)
{
    return d->processNonPoolPacket(buf, notStun, addr);
}

void TurnClient::outgoingDatagramsWritten(int count) { d->udp_datagramsWritten(count); }

QString TurnClient::realm() const
{
    if (d->pool)
        return d->pool->realm();
    else
        return d->realm;
}

void TurnClient::setUsername(const QString &username)
{
    d->user = username;
    if (d->pool)
        d->pool->setUsername(d->user);
}

void TurnClient::setPassword(const QCA::SecureArray &password)
{
    d->pass = password;
    if (d->pool)
        d->pool->setPassword(d->pass);
}

void TurnClient::setRealm(const QString &realm)
{
    d->realm = realm;
    if (d->pool)
        d->pool->setRealm(d->realm);
}

void TurnClient::continueAfterParams()
{
    Q_ASSERT(d->pool);
    d->pool->continueAfterParams();
}

void TurnClient::close() { d->do_close(); }

StunAllocate *TurnClient::stunAllocate() { return d->allocate; }

void TurnClient::addChannelPeer(const TransportAddress &addr) { d->addChannelPeer(addr); }

int TurnClient::packetsToRead() const { return d->in.count(); }

int TurnClient::packetsToWrite() const { return d->outPending.count() + d->outPendingWrite; }

QByteArray TurnClient::read(TransportAddress &addr)
{
    if (!d->in.isEmpty()) {
        Private::Packet p = d->in.takeFirst();
        addr              = p.addr;
        return p.data;
    } else
        return QByteArray();
}

void TurnClient::write(const QByteArray &buf, const TransportAddress &addr) { d->writeOrQueue(buf, addr); }

QString TurnClient::errorString() const { return d->errorString; }

void TurnClient::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    if (d->pool)
        d->pool->setDebugLevel((StunTransactionPool::DebugLevel)level);
}

void TurnClient::changeThread(QThread *thread)
{
    if (d->pool)
        d->pool->moveToThread(thread);
    moveToThread(thread);
}
} // namespace XMPP

#include "turnclient.moc"
