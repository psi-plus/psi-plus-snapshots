/*
 * jignle-ibb.cpp - Jingle In-Band Bytestream transport
 * Copyright (C) 2019  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "jingle-ibb.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_ibb.h"

#include <QTimer>

namespace XMPP {
namespace Jingle {
namespace IBB {

const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));

class Connection : public XMPP::Jingle::Connection
{
    Q_OBJECT

public:
    Client *client;
    Jid peer;
    QString sid;
    size_t _blockSize;
    IBBConnection *connection = nullptr;

    bool offerSent = false;
    bool offerReceived = false;
    bool closing = false;
    bool finished = false;

    Connection(Client *client, const Jid &jid, const QString &sid, size_t blockSize) :
        client(client),
        peer(jid),
        sid(sid),
        _blockSize(blockSize)
    {

    }

    void setConnection(IBBConnection *c)
    {
        connection = c;
        connect(c, &IBBConnection::readyRead, this, &Connection::readyRead);
        connect(c, &IBBConnection::bytesWritten, this, &Connection::bytesWritten);
        connect(c, &IBBConnection::connectionClosed, this, &Connection::handleIBBClosed);
        connect(c, &IBBConnection::delayedCloseFinished, this, &Connection::handleIBBClosed);
        connect(c, &IBBConnection::aboutToClose, this, &Connection::aboutToClose);
        connect(c, &IBBConnection::connected, this, [this](){ setOpenMode(connection->openMode()); emit connected(); });
    }

    size_t blockSize() const
    {
        return _blockSize;
    }

    qint64 bytesAvailable() const
    {
        return XMPP::Jingle::Connection::bytesAvailable() + (connection? connection->bytesAvailable() : 0);
    }

    qint64 bytesToWrite() const
    {
        return XMPP::Jingle::Connection::bytesToWrite() + (connection? connection->bytesToWrite() : 0);
    }

    void close()
    {
        if (connection) {
            connection->close();
            setOpenMode(connection->openMode());
        } else {
            XMPP::Jingle::Connection::close();
            emit connectionClosed();
        }
    }

signals:
    void connected();

protected:
    qint64 writeData(const char *data, qint64 maxSize)
    {
        return connection->write(data, maxSize);
    }

    qint64 readData(char *data, qint64 maxSize)
    {
        quint64 ret = connection->read(data, maxSize);
        if (closing && !bytesAvailable()) {
            postCloseAllDataRead();
        }
        return ret;
    }

private:

    void handleIBBClosed()
    {
        closing = true;
        if (bytesAvailable())
            setOpenMode(QIODevice::ReadOnly);
        else
            postCloseAllDataRead();
    }

    void postCloseAllDataRead()
    {
        closing = false;
        finished = true;
        connection->deleteLater();
        connection = nullptr;
        setOpenMode(QIODevice::NotOpen);
        emit connectionClosed();
    }
};

struct Transport::Private
{
    Transport *q = nullptr;
    Pad::Ptr pad;
    QMap<QString,QSharedPointer<Connection>> connections;
    QList<QSharedPointer<Connection>> readyConnections;
    QSharedPointer<Connection> lastOfferedConnection;
    size_t defaultBlockSize = 4096;
    bool started = false;
    bool initialOfferSent = false; // if we ever sent anything

    void checkAndStartConnection(const QSharedPointer<Connection> &c)
    {
        if (!c->connection && !c->finished && c->offerReceived && c->offerSent && pad->session()->role() == Origin::Initiator) {
            auto con = pad->session()->manager()->client()->ibbManager()->createConnection();
            auto ibbcon = static_cast<IBBConnection*>(con);
            ibbcon->setPacketSize(defaultBlockSize);
            c->setConnection(ibbcon);
            ibbcon->connectToJid(pad->session()->peer(), c->sid);
        }
    }

    void handleConnected(const QSharedPointer<Connection> &c)
    {
        if (c) {
            readyConnections.append(c);
            emit q->connected();
        }
    }

    OutgoingTransportInfoUpdate makeOffer(const QSharedPointer<Connection> &connection)
    {
        OutgoingTransportInfoUpdate upd;
        if (!connection) {
            return upd;
        }

        auto doc = pad->session()->manager()->client()->doc();

        QDomElement tel = doc->createElementNS(NS, "transport");
        tel.setAttribute(QStringLiteral("sid"), connection->sid);
        tel.setAttribute(QString::fromLatin1("block-size"), qulonglong(connection->_blockSize));

        upd = OutgoingTransportInfoUpdate{tel, [this, connection]() mutable {
            if (started)
                checkAndStartConnection(connection);
        }};

        lastOfferedConnection = connection;
        connection->offerSent = true;
        return upd;
    }
};

Transport::Transport(const TransportManagerPad::Ptr &pad) :
    d(new Private)
{
    d->q = this;
    d->pad = pad.staticCast<Pad>();
    connect(pad->manager(), &TransportManager::abortAllRequested, this, [this](){
        for(auto &c: d->connections) {
            c->close();
        }
        //d->aborted = true;
        emit failed();
    });
}

Transport::Transport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl) :
    d(new Private)
{
    d->q = this;
    d->pad = pad.staticCast<Pad>();
    update(transportEl);
    if (d->connections.isEmpty()) {
        d.reset();
        return;
    }
}

Transport::~Transport()
{
    // we have to mark all of them as finished just in case they are captured somewhere else
    if (d) {
        for (auto &c: d->connections) {
            c->finished = true;
        }
    }
}

TransportManagerPad::Ptr Transport::pad() const
{
    return d->pad;
}

void Transport::prepare()
{
    if (d->connections.isEmpty()) { // seems like outgoing
        auto conn = d->pad->makeConnection(QString(), d->defaultBlockSize);
        auto ibbConn = conn.staticCast<Connection>();
        connect(ibbConn.data(), &Connection::connected, this, [this](){
            auto c = static_cast<Connection*>(sender());
            d->handleConnected(d->connections.value(c->sid));
        });
        d->connections.insert(ibbConn->sid, ibbConn);

        connect(ibbConn.data(), &Connection::connectionClosed, this, [this](){
            Connection *c = static_cast<Connection*>(sender());
            d->connections.remove(c->sid);
            QMutableListIterator<QSharedPointer<Connection>> it(d->readyConnections);
            while (it.hasNext()) {
                auto &p = it.next();
                if (p.data() == c) {
                    it.remove();
                    break;
                }
            }
        });
    }
    emit updated();
}

void Transport::start()
{
    d->started = true;

    for (auto &c: d->connections) {
        d->checkAndStartConnection(c);
    }
}

bool Transport::update(const QDomElement &transportEl)
{
    QString sid = transportEl.attribute(QString::fromLatin1("sid"));
    if (sid.isEmpty()) {
        return false;
    }

    size_t bs_final = d->defaultBlockSize;
    auto bs = transportEl.attribute(QString::fromLatin1("block-size"));
    if (!bs.isEmpty()) {
        size_t bsn = bs.toULongLong();
        if (bsn && bsn <= bs_final) {
            bs_final = bsn;
        }
    }

    auto it = d->connections.find(sid);
    if (it == d->connections.end()) {
        auto c = d->pad->makeConnection(sid, bs_final);
        if (c) {
            auto ibbc = c.staticCast<Connection>();
            it = d->connections.insert(ibbc->sid, ibbc);
            connect(ibbc.data(), &Connection::connected, this, [this](){
                auto c = static_cast<Connection*>(sender());
                d->handleConnected(d->connections.value(c->sid));
            });
        } else {
            qWarning("failed to create IBB connection");
            return false;
        }
    } else {
        if (bs_final < (*it)->_blockSize) {
            (*it)->_blockSize = bs_final;
        }
    }

    (*it)->offerReceived = true;
    if (d->started) {
        auto c = it.value();
        QTimer::singleShot(0, this, [this,c]() mutable { d->checkAndStartConnection(c); });
    }
    return true;
}


bool Transport::isInitialOfferReady() const
{
    return isValid() && (hasUpdates() || d->initialOfferSent);
}

OutgoingTransportInfoUpdate Transport::takeInitialOffer()
{
    auto upd = takeOutgoingUpdate();
    if (std::get<0>(upd).isNull() && d->lastOfferedConnection) {
        return d->makeOffer(d->lastOfferedConnection);
    }
    return upd;
}

bool Transport::hasUpdates() const
{
    for (auto &c: d->connections) {
        if (!c->offerSent) {
            return true;
        }
    }
    return false;
}

OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate()
{
    OutgoingTransportInfoUpdate upd;
    if (!isValid()) {
        return upd;
    }

    QSharedPointer<Connection> connection;
    for (auto &c: d->connections) {
        if (!c->offerSent) {
            connection = c;
            break;
        }
    }
    return d->makeOffer(connection);
}

bool Transport::isValid() const
{
    return d;
}

Transport::Features Transport::features() const
{
    return AlwaysConnect | Reliable | Slow;
}

Connection::Ptr Transport::connection() const
{
    return d->readyConnections.isEmpty()? Connection::Ptr() : d->readyConnections.takeFirst().staticCast<XMPP::Jingle::Connection>();
}

Pad::Pad(Manager *manager, Session *session)
{
    _manager = manager;
    _session = session;
}

QString Pad::ns() const
{
    return NS;
}

Session *Pad::session() const
{
    return _session;
}

TransportManager *Pad::manager() const
{
    return _manager;
}

Connection::Ptr Pad::makeConnection(const QString &sid, size_t blockSize)
{
    return _manager->makeConnection(_session->peer(), sid, blockSize);
}

struct Manager::Private
{
    QHash<QPair<Jid,QString>,QSharedPointer<Connection>> connections;
    XMPP::Jingle::Manager *jingleManager = nullptr;
};

Manager::Manager(QObject *parent) :
    TransportManager(parent),
    d(new Private)
{

}

Manager::~Manager()
{
    if (d->jingleManager)
        d->jingleManager->unregisterTransport(NS);
}

Transport::Features Manager::features() const
{
    return Transport::AlwaysConnect | Transport::Reliable | Transport::Slow;
}

void Manager::setJingleManager(XMPP::Jingle::Manager *jm)
{
    d->jingleManager = jm;
}

QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad)
{
    return QSharedPointer<XMPP::Jingle::Transport>(new Transport(pad));
}

QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl)
{
    auto t = new Transport(pad, transportEl);
    QSharedPointer<XMPP::Jingle::Transport> ret(t);
    if (t->isValid()) {
        return ret;
    }
    return QSharedPointer<XMPP::Jingle::Transport>();
}

TransportManagerPad* Manager::pad(Session *session)
{
    return new Pad(this, session);
}

void Manager::closeAll()
{
    emit abortAllRequested();
}

XMPP::Jingle::Connection::Ptr Manager::makeConnection(const Jid &peer, const QString &sid, size_t blockSize)
{
    if (!sid.isEmpty() && d->connections.contains(qMakePair(peer, sid))) {
        qWarning("sid %s was already registered for %s", qPrintable(sid), qPrintable(peer.full()));
        return Connection::Ptr();
    }
    QString s(sid);
    if (s.isEmpty()) {
        QPair<Jid,QString> key;
        do {
            s = QString("ibb_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
            key = qMakePair(peer, s);
        } while (d->connections.contains(key));
    }
    auto conn = QSharedPointer<Connection>::create(d->jingleManager->client(), peer, s, blockSize);
    d->connections.insert(qMakePair(peer, s), conn);
    connect(conn.data(), &Connection::connectionClosed, this, [this](){
        Connection *c = static_cast<Connection*>(sender());
        d->connections.remove(qMakePair(c->peer, c->sid));
    });

    return conn.staticCast<XMPP::Jingle::Connection>();
}

bool Manager::handleIncoming(IBBConnection *c)
{
    auto conn = d->connections.value(qMakePair(c->peer(), c->sid()));
    if (conn) {
        conn->setConnection(c);
        QTimer::singleShot(0, c, &IBBConnection::accept);
        return true;
    }
    return false;
}

} // namespace IBB
} // namespace Jingle
} // namespace XMPP

#include "jingle-ibb.moc"
