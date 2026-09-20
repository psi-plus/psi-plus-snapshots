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
#include "jingle-session.h"

#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_ibb.h"
#include "xmpp_task.h"

#include <QPointer>
#include <QTimer>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif

template <class T> constexpr std::add_const_t<T> &as_const(T &t) noexcept { return t; }

namespace XMPP { namespace Jingle { namespace IBB {
    const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));

    class Connection : public XMPP::Jingle::Connection {
        Q_OBJECT

    public:
        Client        *client;
        Jid            peer;
        QString        sid;
        size_t         _blockSize;
        IBBConnection *connection = nullptr;
        State          state      = State::Created;
        Origin         creator    = Origin::None;

        //        bool offerSent     = false;
        //        bool offerReceived = false;
        //        bool closing       = false;
        //        bool finished      = false;

        Connection(Client *client, const Jid &jid, const QString &sid, size_t blockSize) :
            client(client), peer(jid), sid(sid), _blockSize(blockSize)
        {
        }

        void setConnection(IBBConnection *c)
        {
            c->setParent(this);
            connection = c;
            connect(c, &IBBConnection::readyRead, this, &Connection::readyRead);
            connect(c, &IBBConnection::bytesWritten, this, &Connection::bytesWritten);
            connect(c, &IBBConnection::connectionClosed, this, &Connection::handleIBBClosed);
            connect(c, &IBBConnection::delayedCloseFinished, this, &Connection::handleIBBClosed);
            connect(c, &IBBConnection::aboutToClose, this, &Connection::aboutToClose);
            connect(c, &IBBConnection::connected, this, &Connection::handleConnnected);
            connect(c, &IBBConnection::error, this, [this](int error) {
                setError(error);
                handleIBBClosed();
            });
        }

        void handleConnnected()
        {
            state = State::Active;
            setOpenMode(connection->openMode());
            emit connected();
        }

        TransportFeatures features() const
        {
            return TransportFeature::DataOriented | TransportFeature::StreamOriented | TransportFeature::Ordered
                | TransportFeature::Reliable;
        }

        size_t blockSize() const { return _blockSize; }

        qint64 bytesAvailable() const
        {
            return XMPP::Jingle::Connection::bytesAvailable() + (connection ? connection->bytesAvailable() : 0);
        }

        qint64 bytesToWrite() const
        {
            return XMPP::Jingle::Connection::bytesToWrite() + (connection ? connection->bytesToWrite() : 0);
        }

        void close()
        {
            if (state >= State::Finishing)
                return;

            if (connection) {
                state = State::Finishing;
                connection->close();
                setOpenMode(connection->openMode());
                if (!connection->isOpen() && !bytesAvailable())
                    postCloseAllDataRead();
            } else {
                state = State::Finished;
                XMPP::Jingle::Connection::close();
                emit connectionClosed();
            }
        }

    protected:
        qint64 writeData(const char *data, qint64 maxSize) { return connection->write(data, maxSize); }

        qint64 readDataInternal(char *data, qint64 maxSize)
        {
            qint64 ret = connection->read(data, maxSize);
            if (state == State::Finishing && !bytesAvailable()) {
                postCloseAllDataRead();
            }
            return ret;
        }

    private:
        void handleIBBClosed()
        {
            if (!connection || state == State::Finished)
                return;
            state = State::Finishing;
            if (bytesAvailable())
                setOpenMode(QIODevice::ReadOnly);
            else
                postCloseAllDataRead();
        }

        void postCloseAllDataRead()
        {
            state = State::Finished;
            connection->deleteLater();
            connection = nullptr;
            setOpenMode(QIODevice::NotOpen);
            emit connectionClosed();
        }
    };

    class PreparedIbbUpdate final : public XMPP::Jingle::Transport::PreparedUpdate {
    public:
        enum class Kind { NewStream, Existing, Noop };

        QString                  sid;
        size_t                   blockSize = 0;
        Kind                     kind      = Kind::NewStream;
        QWeakPointer<Connection> expected;
    };

    struct Transport::Private {
        Transport                                *q = nullptr;
        QMap<QString, QSharedPointer<Connection>> connections;
        size_t                                    defaultBlockSize = 4096;
        bool                                      started          = false;

        ~Private() { qDebug("destroying ibb private"); }

        void checkAndStartConnection(const QSharedPointer<Connection> &c)
        {
            if (c->connection || c->state != State::Accepted)
                return;

            c->state = State::Connecting;
            if (q->_pad->session()->role() == Origin::Initiator) {
                auto con    = q->_pad->session()->manager()->client()->ibbManager()->createConnection();
                auto ibbcon = static_cast<IBBConnection *>(con);
                ibbcon->setPacketSize(int(c->blockSize()));
                c->setConnection(ibbcon);
                ibbcon->connectToJid(q->_pad->session()->peer(), c->sid);
            } // else we are waiting for incoming open
        }

        QSharedPointer<Connection> newStream(const QString &sid, std::size_t blockSize, Origin creator)
        {
            auto conn    = q->_pad.staticCast<Pad>()->makeConnection(sid, blockSize);
            auto ibbConn = conn.staticCast<Connection>();
            if (!ibbConn)
                return ibbConn;
            ibbConn->creator = creator;
            QObject::connect(ibbConn.data(), &Connection::connected, q, [this]() {
                if (q->_state == State::Connecting) {
                    q->setState(State::Active);
                }
            });
            QObject::connect(ibbConn.data(), &Connection::connectionClosed, q, [this]() {
                Connection *c = static_cast<Connection *>(q->sender());
                connections.remove(c->sid);
            });

            return ibbConn;
        }
    };

    Transport::Transport(const TransportManagerPad::Ptr &pad, Origin creator) :
        XMPP::Jingle::Transport(pad, creator), d(new Private)
    {
        d->q = this;
        connect(pad->manager(), &TransportManager::abortAllRequested, this, [this]() {
            while (d->connections.size()) {
                d->connections.first()->close();
            }
            // d->aborted = true;
            onFinish(Reason::Cancel);
        });
    }

    Transport::~Transport()
    {
        // we have to mark all of them as finished just in case they are captured somewhere else
        qDebug("jingle-ibb: destroy");
        auto connections = d->connections.values();
        for (auto &c : connections) {
            c->close();
        }
    }

    void Transport::prepare()
    {
        setState(State::ApprovedToSend);
        auto it = d->connections.begin();
        while (it != d->connections.end()) {
            auto &c = it.value();
            if (c->isRemote() && !notifyIncomingConnection(c)) {
                it = d->connections.erase(it);
                continue;
            }
            c->state = State::ApprovedToSend;
            ++it;
        }
        if (d->connections.isEmpty()) {
            onFinish(Reason::GeneralError, QLatin1String("no connections registered"));
        } else
            emit updated();
    }

    void Transport::start()
    {
        setState(State::Connecting);

        for (auto &c : d->connections) {
            d->checkAndStartConnection(c);
        }
    }

    Transport::PrepareUpdateResult Transport::prepareUpdate(const QDomElement &transportEl)
    {
        if (_state == State::Finished) {
            qWarning("The IBB transport has finished already");
            return { PrepareUpdateStatus::Invalid, {}, {} };
        }

        const QString sid = transportEl.attribute(QString::fromLatin1("sid"));
        if (sid.isEmpty()) {
            qWarning("empty SID");
            return { PrepareUpdateStatus::Invalid, {}, {} };
        }

        size_t blockSize = d->defaultBlockSize;
        const auto bs    = transportEl.attribute(QString::fromLatin1("block-size"));
        if (!bs.isEmpty()) {
            const size_t parsed = bs.toULongLong();
            if (parsed && parsed <= blockSize)
                blockSize = parsed;
        }

        auto prepared       = std::make_unique<PreparedIbbUpdate>();
        prepared->sid       = sid;
        prepared->blockSize = blockSize;

        const auto it = d->connections.constFind(sid);
        if (it == d->connections.cend()) {
            prepared->kind = PreparedIbbUpdate::Kind::NewStream;
            return { PrepareUpdateStatus::Ready, std::move(prepared), {} };
        }

        prepared->expected = it.value().toWeakRef();
        if (it.value()->creator != _pad->session()->role() || it.value()->state != State::Pending) {
            if (it.value()->state >= State::Accepted && it.value()->state <= State::Active) {
                prepared->kind = PreparedIbbUpdate::Kind::Noop;
                return { PrepareUpdateStatus::Ready, std::move(prepared), {} };
            }
            qWarning("Unexpected IBB answer");
            return { PrepareUpdateStatus::Invalid, {}, {} };
        }

        prepared->kind = PreparedIbbUpdate::Kind::Existing;
        return { PrepareUpdateStatus::Ready, std::move(prepared), {} };
    }

    bool Transport::commitPreparedUpdate(PreparedUpdatePtr update)
    {
        auto prepared = dynamic_cast<PreparedIbbUpdate *>(update.get());
        if (!prepared || _state == State::Finished)
            return false;

        QPointer<Session> session(_pad->session());
        auto              it = d->connections.find(prepared->sid);

        if (prepared->kind == PreparedIbbUpdate::Kind::NewStream) {
            if (it != d->connections.end())
                return false;

            auto connection = d->newStream(prepared->sid, prepared->blockSize, _pad->session()->peerRole());
            if (!connection) {
                qWarning("failed to create IBB connection");
                return false;
            }
            connection->setRemote(true);
            connection->state = State::Pending;

            bool inserted = false;
            if (_state == State::Created && isRemote()) {
                d->connections.insert(prepared->sid, connection);
                inserted = true;
                // Publish state only after the new connection is fully installed:
                // stateChanged() is an external reentrancy boundary.
                setState(State::Pending);
                if (!session)
                    return true;
            } else if (!wasAccepted() || notifyIncomingConnection(connection)) {
                if (!session)
                    return true;
                d->connections.insert(prepared->sid, connection);
                inserted = true;
            }

            if (session && inserted && _state >= State::Connecting) {
                QTimer::singleShot(0, this, [this, connection]() mutable { d->checkAndStartConnection(connection); });
            }
            return true;
        }

        auto expected = prepared->expected.lock();
        if (!expected || it == d->connections.end() || it.value() != expected)
            return false;

        if (prepared->kind == PreparedIbbUpdate::Kind::Noop)
            return true;

        if (expected->creator != _pad->session()->role() || expected->state != State::Pending)
            return false;

        if (prepared->blockSize < expected->_blockSize)
            expected->_blockSize = prepared->blockSize;
        // Complete the connection mutation before publishing the transport state.
        expected->state = State::Accepted;
        if (_creator == _pad->session()->role()) {
            setState(State::Accepted);
            if (!session)
                return true;
        }

        if (session && _state >= State::Connecting) {
            QTimer::singleShot(0, this, [this, expected]() mutable { d->checkAndStartConnection(expected); });
        }
        return true;
    }

    bool Transport::update(const QDomElement &transportEl)
    {
        auto prepared = prepareUpdate(transportEl);
        return prepared && commitPreparedUpdate(std::move(prepared.update));
    }

    bool Transport::hasUpdates() const
    {
        for (auto &c : d->connections) {
            if (c->state == State::ApprovedToSend) {
                return true;
            }
        }
        return false;
    }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate(bool ensureTransportElement)
    {
        OutgoingTransportInfoUpdate upd;
        if (!isValid()) {
            return upd;
        }

        auto doc = _pad->session()->manager()->client()->doc();
        auto it  = std::find_if(d->connections.begin(), d->connections.end(),
                                [](auto &c) { return c->state == State::ApprovedToSend; });

        if (it == d->connections.end()) {
            if (ensureTransportElement) {
                // a really dirty workaround here which ignore the fact IBB may have more then one transport for
                // a single content
                it = std::find_if(d->connections.begin(), d->connections.end(),
                                  [](auto &c) { return c->state > State::ApprovedToSend; });
                if (it == d->connections.end()) {
                    return upd;
                }
                QDomElement tel = doc->createElementNS(NS, "transport");
                tel.setAttribute(QStringLiteral("sid"), it.value()->sid);
                tel.setAttribute(QString::fromLatin1("block-size"), qulonglong(it.value()->_blockSize));
                std::get<0>(upd) = tel;
            }
            return upd;
        }

        auto connection   = it.value();
        connection->state = State::Unacked;

        QDomElement tel = doc->createElementNS(NS, "transport");
        tel.setAttribute(QStringLiteral("sid"), connection->sid);
        tel.setAttribute(QString::fromLatin1("block-size"), qulonglong(connection->_blockSize));

        if (_state == State::ApprovedToSend) {
            setState(State::Unacked);
        }
        upd = OutgoingTransportInfoUpdate {
            tel,
            [this, guard = QPointer<Transport>(this), connection](Task *task) mutable {
                if (!guard || !task || !task->success() || connection->state != State::Unacked)
                    return;

                if (connection->creator == _pad->session()->role()) {
                    connection->state = State::Pending;
                } else {
                    connection->state = State::Accepted;
                }

                if (_state == State::Unacked) {
                    setState(_creator == _pad->session()->role() ? State::Pending : State::Accepted);
                }
                if (guard && _state >= State::Connecting)
                    d->checkAndStartConnection(connection);
            }
        };

        return upd;
    }

    bool Transport::isValid() const { return bool(d); }

    TransportFeatures Transport::features() const
    {
        return TransportFeature::AlwaysConnect | TransportFeature::Reliable | TransportFeature::StreamOriented
            | TransportFeature::DataOriented;
    }

    int Transport::maxSupportedChannelsPerComponent(TransportFeatures) const { return -1; }

    Connection::Ptr Transport::addChannel(TransportFeatures features, const QString &id, int)
    {
        if (features & TransportFeature::LiveOriented)
            return {};
        auto ibbConn = d->newStream(QString(), d->defaultBlockSize, _pad->session()->role());
        ibbConn->setId(id);
        d->connections.insert(ibbConn->sid, ibbConn);
        return ibbConn;
    }

    QList<XMPP::Jingle::Connection::Ptr> Transport::channels() const
    {
        QList<Connection::Ptr> ret;
        ret.reserve(d->connections.size());
        for (auto const &v : as_const(d->connections)) {
            ret.append(v);
        }
        return ret;
    }

    Pad::Pad(Manager *manager, Session *session)
    {
        _manager = manager;
        _session = session;
    }

    QString Pad::ns() const { return NS; }

    Session *Pad::session() const { return _session; }

    TransportManager *Pad::manager() const { return _manager; }

    Connection::Ptr Pad::makeConnection(const QString &sid, size_t blockSize)
    {
        return _manager->makeConnection(_session->peer(), sid, blockSize);
    }

    struct Manager::Private {
        QHash<QPair<Jid, QString>, QSharedPointer<Connection>> connections;
        XMPP::Jingle::Manager                                 *jingleManager = nullptr;
    };

    Manager::Manager(QObject *parent) : TransportManager(parent), d(new Private) { }

    Manager::~Manager()
    {
        if (d->jingleManager)
            d->jingleManager->unregisterTransport(NS);
    }

    TransportFeatures Manager::features() const
    {
        return TransportFeature::AlwaysConnect | TransportFeature::Reliable | TransportFeature::Ordered
            | TransportFeature::DataOriented;
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { d->jingleManager = jm; }

    QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, Origin creator)
    {
        return QSharedPointer<Transport>::create(pad, creator).staticCast<XMPP::Jingle::Transport>();
    }

    TransportManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    QStringList Manager::discoFeatures() const { return { NS }; }

    XMPP::Jingle::Connection::Ptr Manager::makeConnection(const Jid &peer, const QString &sid, size_t blockSize)
    {
        if (!sid.isEmpty() && d->connections.contains(qMakePair(peer, sid))) {
            qWarning("sid %s was already registered for %s", qPrintable(sid), qPrintable(peer.full()));
            return Connection::Ptr();
        }
        QString s(sid);
        if (s.isEmpty()) {
            QPair<Jid, QString> key;
            do {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
                s = QString("ibb_%1").arg(QRandomGenerator::global()->generate() & 0xffff, 4, 16, QChar('0'));
#else
                s = QString("ibb_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
#endif
                key = qMakePair(peer, s);
            } while (d->connections.contains(key));
        }
        auto conn = QSharedPointer<Connection>::create(d->jingleManager->client(), peer, s, blockSize);
        d->connections.insert(qMakePair(peer, s), conn);
        connect(conn.data(), &Connection::connectionClosed, this, [this]() {
            Connection *c = static_cast<Connection *>(sender());
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
