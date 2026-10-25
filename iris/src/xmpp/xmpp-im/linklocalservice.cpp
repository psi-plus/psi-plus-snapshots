/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "xmpp_linklocalservice.h"
#include "xmpp_linklocalstream.h"
#include <QDomDocument>
#include <QLoggingCategory>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#ifdef IRIS_HAVE_LINKLOCAL
#include <qjdnsmulticast.h>
#endif

Q_LOGGING_CATEGORY(linkLocalLog, "iris.linklocal", QtWarningMsg)

namespace XMPP {
namespace {
    const QByteArray Service("_presence._tcp"), Browse("_presence._tcp.local.");
    constexpr int    MaxQueued = 256, MaxBytes = 1024 * 1024, MaxPeers = 4096, MaxConnections = 128;
}
class LinkLocalService::Private : public QObject {
public:
    LinkLocalService       *q;
    mutable QDomDocument    document;
    Jid                     base, local;
    QHostAddress            address;
    QString                 displayName, failure;
    Status                  status;
    bool                    running = false, stopping = false, announced = false, dnsDone = false, closed = false;
    int                     hostNumber = 0, userNumber = 0, generation = 0, queuedBytes = 0, queuedCount = 0;
    QQueue<Stanza>          received;
    int                     receivedBytes = 0;
    QSet<LinkLocalStream *> streams;
    QHash<QString, QPointer<LinkLocalStream>> channels;
    QTcpServer                                server;
#ifdef IRIS_HAVE_LINKLOCAL
    QJDnsMulticast             *dns    = nullptr;
    QJDnsSharedRequest         *browse = nullptr, *txtPublication = nullptr;
    QList<QJDnsSharedRequest *> publications;
    struct Peer : QObject {
        explicit Peer(QObject *parent) : QObject(parent) { }
        Jid                       jid;
        QByteArray                instance;
        QString                   name;
        Status                    status;
        QPointer<LinkLocalStream> opening;
        QPointer<QObject>         resolution;
        QQueue<Stanza>            pending;
    };
    QHash<QString, Peer *> peers;
#endif
    explicit Private(LinkLocalService *owner) : QObject(owner), q(owner)
    {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                if (stopping || streams.size() >= MaxConnections) {
                    socket->deleteLater();
                    continue;
                }
                socket->setParent(this);
                attach(socket, {}, LinkLocalStream::Direction::Recipient);
            }
        });
    }
    void fail(const QString &text)
    {
        failure = text;
        qCWarning(linkLocalLog) << "Account transport failed:" << text;
        QTimer::singleShot(0, this, [this] {
            QPointer<LinkLocalService> guard(q);
            emit                       q->error(Stream::ErrCustom);
            if (guard)
                guard->close();
        });
    }
    void maybeClosed()
    {
        if (stopping && dnsDone && streams.isEmpty() && !closed) {
            closed = true;
            emit q->connectionClosed();
        }
    }
#ifdef IRIS_HAVE_LINKLOCAL
    QJDns::Record txtRecord() const
    {
        QJDns::Record txt;
        txt.owner       = QJDnsMulticast::serviceName(local.full(), Service);
        txt.type        = QJDns::Txt;
        txt.haveKnown   = true;
        const auto show = status.type() == Status::DND ? QByteArray("dnd")
            : status.isAway()                          ? QByteArray("away")
                                                       : QByteArray("avail");
        txt.texts       = { "txtvers=1", "status=" + show, "port.p2pj=" + QByteArray::number(server.serverPort()) };
        // A DNS TXT character-string is limited to 255 octets, including key=.
        const auto addText = [&txt](const QByteArray &key, const QString &text) {
            auto bytes = text.toUtf8();
            if (key.size() + bytes.size() > 255) {
                int length = 255 - key.size();
                while (length > 0 && (uchar(bytes.at(length)) & 0xc0) == 0x80)
                    --length;
                bytes.truncate(length);
            }
            txt.texts.append(key + bytes);
        };
        addText("nick=", displayName);
        addText("msg=", status.status());
        txt.ttl = 4500;
        return txt;
    }
    void announce()
    {
        if (stopping)
            return;
        const int serial = ++generation;
        qDeleteAll(publications);
        publications.clear();
        txtPublication      = nullptr;
        local               = Jid(base.node() + (userNumber ? "-" + QString::number(userNumber) : QString()),
                                  base.domain() + (hostNumber ? "-" + QString::number(hostNumber) : QString()));
        const auto instance = QJDnsMulticast::serviceName(local.full(), Service);
        if (instance.isEmpty()) {
            fail(tr("The serverless name is too long for DNS-SD."));
            return;
        }
        const QPointer<Private> guard(this);
        emit                    q->identityChanged(local);
        if (!guard)
            return;
        const auto    host = local.domain().toLatin1() + ".local.";
        QJDns::Record ip, srv;
        ip.owner      = host;
        ip.type       = address.protocol() == QAbstractSocket::IPv6Protocol ? QJDns::Aaaa : QJDns::A;
        ip.haveKnown  = true;
        ip.address    = address;
        ip.ttl        = 120;
        srv.owner     = instance;
        srv.type      = QJDns::Srv;
        srv.haveKnown = true;
        srv.name      = host;
        srv.port      = server.serverPort();
        srv.priority = srv.weight = 0;
        srv.ttl                   = 120;
        auto remaining            = std::make_shared<int>(3);
        for (const auto &record : { ip, srv, txtRecord() }) {
            auto *publication = dns->publish(record);
            publications.append(publication);
            if (record.type == QJDns::Txt)
                txtPublication = publication;
            auto confirmed = std::make_shared<bool>(false);
            connect(publication, &QJDnsSharedRequest::resultsReady, this,
                    [this, publication, serial, remaining, confirmed, host, record, instance] {
                        if (serial != generation || stopping)
                            return;
                        if (!publication->success()) {
                            if (!announced && publication->error() == QJDnsSharedRequest::ErrorConflict) {
                                if (record.owner == host)
                                    ++hostNumber;
                                else
                                    ++userNumber;
                                ++generation;
                                QTimer::singleShot(0, this, [this] { announce(); });
                            } else
                                fail(tr("Cannot publish the serverless address on this network."));
                            return;
                        }
                        if (*confirmed)
                            return;
                        *confirmed = true;
                        if (--*remaining)
                            return;
                        QJDns::Record ptr;
                        ptr.owner          = Browse;
                        ptr.type           = QJDns::Ptr;
                        ptr.haveKnown      = true;
                        ptr.name           = instance;
                        ptr.ttl            = 4500;
                        auto *announcement = dns->publish(ptr, QJDns::Shared);
                        publications.prepend(announcement);
                        connect(announcement, &QJDnsSharedRequest::resultsReady, this, [this, announcement, serial] {
                            if (serial != generation || stopping)
                                return;
                            if (!announcement->success()) {
                                fail(tr("Cannot announce serverless presence."));
                                return;
                            }
                            if (!announced) {
                                announced = true;
                                qCDebug(linkLocalLog) << "Online" << local.full() << address << server.serverPort();
                                emit q->online();
                            }
                        });
                    });
        }
    }
    void clearPeerQueue(Peer *peer)
    {
        while (!peer->pending.isEmpty()) {
            queuedBytes -= peer->pending.dequeue().toString().toUtf8().size();
            --queuedCount;
        }
    }
    void peerFailed(Peer *peer, const QString &text)
    {
        const Jid jid = peer->jid;
        clearPeerQueue(peer);
        peer->opening = nullptr;
        if (peer->resolution)
            peer->resolution->deleteLater();
        peer->resolution = nullptr;
        emit q->peerError(jid, text);
    }
    void discover(const QJDns::Record &record)
    {
        const Jid jid(QJDnsMulticast::instanceName(record.name, Service));
        if (record.type != QJDns::Ptr || !jid.isValid() || !jid.isBare() || jid.node().isEmpty() || jid == local)
            return;
        auto *peer = peers.value(jid.full());
        if (!record.ttl) {
            if (!peer || peer->instance != record.name)
                return;
            peers.remove(jid.full());
            clearPeerQueue(peer);
            if (peer->resolution)
                peer->resolution->deleteLater();
            peer->resolution = nullptr;
            if (peer->opening)
                peer->opening->close();
            peer->opening = nullptr;
            if (auto channel = channels.take(jid.full()))
                channel->close();
            peer->deleteLater();
            emit q->peerChanged(jid, peer->name, Status(Status::Offline));
            return;
        }
        if (peer || peers.size() >= MaxPeers)
            return;
        peer           = new Peer(this);
        peer->jid      = jid;
        peer->instance = record.name;
        peer->name     = jid.node();
        peers.insert(jid.full(), peer);
        // Presence is DNS-SD TXT, not XML presence. Maintain this subscription
        // only while the PTR is present; SRV and addresses are resolved on demand.
        auto *txt = dns->subscribe(record.name, QJDns::Txt, peer);
        connect(txt, &QJDnsSharedRequest::resultsReady, peer, [this, peer, txt] {
            if (stopping || !txt->success())
                return;
            for (const auto &answer : txt->results()) {
                if (!answer.ttl || answer.type != QJDns::Txt)
                    continue;
                QHash<QByteArray, QByteArray> fields;
                for (const auto &field : answer.texts) {
                    const int equal = field.indexOf('=');
                    if (equal < 1)
                        continue;
                    const auto key = field.left(equal).toLower();
                    if (!fields.contains(key))
                        fields.insert(key, field.mid(equal + 1));
                }
                peer->name = QString::fromUtf8(fields.value("nick"));
                if (peer->name.isEmpty())
                    peer->name = jidName(peer->jid);
                const auto show = fields.value("status");
                peer->status    = Status(show == "dnd"        ? Status::DND
                                             : show == "away" ? Status::Away
                                                              : Status::Online,
                                      QString::fromUtf8(fields.value("msg")));
                emit q->peerChanged(peer->jid, peer->name, peer->status);
            }
        });
        emit q->peerChanged(jid, peer->name, peer->status);
    }
    static QString jidName(const Jid &jid) { return jid.node(); }
    void           resolve(Peer *peer)
    {
        if (peer->resolution || peer->opening || stopping)
            return;
        auto *context    = new QObject(peer);
        peer->resolution = context;
        auto *srv        = dns->subscribe(peer->instance, QJDns::Srv, context);
        connect(srv, &QJDnsSharedRequest::resultsReady, context, [this, peer, srv, context] {
            if (peer->resolution != context)
                return;
            if (!srv->success()) {
                peerFailed(peer, tr("Cannot find the peer's TCP service."));
                return;
            }
            for (const auto &record : srv->results()) {
                if (!record.ttl || record.type != QJDns::Srv || record.port < 1 || record.port > 65535
                    || !record.name.toLower().endsWith(".local."))
                    continue;
                srv->cancel();
                auto connecting = std::make_shared<bool>(false);
                for (auto type : { QJDns::A, QJDns::Aaaa }) {
                    auto *lookup = dns->subscribe(record.name, type, context);
                    connect(lookup, &QJDnsSharedRequest::resultsReady, context,
                            [this, peer, context, lookup, record, connecting] {
                                if (peer->resolution != context || *connecting || !lookup->success())
                                    return;
                                for (const auto &answer : lookup->results()) {
                                    if (!answer.ttl || answer.address.isNull() || answer.address.isMulticast()
                                        || answer.address.protocol() != address.protocol())
                                        continue;
                                    auto remote = answer.address;
                                    if (remote.protocol() == QAbstractSocket::IPv6Protocol) {
                                        const auto ip = remote.toIPv6Address();
                                        if (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80)
                                            remote.setScopeId(dns->networkInterface().name());
                                    }
                                    *connecting  = true;
                                    auto *socket = new QTcpSocket(context);
                                    if (!socket->bind(address, 0)) {
                                        peerFailed(peer, tr("Cannot bind to the selected network interface."));
                                        return;
                                    }
                                    connect(socket, &QTcpSocket::connected, context, [this, peer, context, socket] {
                                        if (peer->resolution != context)
                                            return;
                                        if (streams.size() >= MaxConnections) {
                                            peerFailed(peer, tr("Too many serverless connections."));
                                            return;
                                        }
                                        socket->setParent(this);
                                        peer->opening
                                            = attach(socket, peer->jid, LinkLocalStream::Direction::Initiator);
                                        peer->resolution = nullptr;
                                        context->deleteLater();
                                    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                                    connect(socket, &QTcpSocket::errorOccurred, context,
                                            [this, peer, context](QAbstractSocket::SocketError) {
#else
                            connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), context, [this, peer, context](QAbstractSocket::SocketError) {
#endif
                                                if (peer->resolution != context)
                                                    return;
                                                peerFailed(peer, tr("The peer's TCP connection failed."));
                                            });
                                    socket->connectToHost(remote, quint16(record.port));
                                    return;
                                }
                            });
                }
                return;
            }
        });
        QTimer::singleShot(10000, context, [this, peer, context] {
            if (peer->resolution == context)
                peerFailed(peer, tr("Serverless peer lookup timed out."));
        });
    }
#endif
    LinkLocalStream *attach(QTcpSocket *socket, const Jid &peer, LinkLocalStream::Direction direction)
    {
        auto *stream = new LinkLocalStream(socket, local, peer, direction, this);
        streams.insert(stream);
        connect(stream, &LinkLocalStream::connected, this, [this, stream, direction] {
            const auto key = stream->peerJid().full();
#ifdef IRIS_HAVE_LINKLOCAL
            if (stopping || !peers.contains(key)) {
                stream->close();
                return;
            }
#endif
            const auto previous = channels.value(key);
            if (previous && previous != stream && previous->isConnected()) {
                // Both peers may initiate concurrently. Prefer the same physical
                // connection at both ends; close the other gracefully.
                const bool preferInitiator = local.full() < key;
                if ((direction == LinkLocalStream::Direction::Initiator) != preferInitiator) {
                    stream->close();
                    return;
                }
                previous->close();
            }
            channels.insert(key, stream);
#ifdef IRIS_HAVE_LINKLOCAL
            auto *known = peers.value(key);
            if (known && known->opening == stream)
                known->opening = nullptr;
            while (known && !known->pending.isEmpty()) {
                auto stanza = known->pending.dequeue();
                queuedBytes -= stanza.toString().toUtf8().size();
                --queuedCount;
                stream->write(stanza);
            }
#endif
        });
        connect(stream, &LinkLocalStream::readyRead, this, [this, stream] {
            while (stream->stanzaAvailable()) {
                auto      stanza = stream->read();
                const int bytes  = stanza.toString().toUtf8().size();
                if (received.size() >= MaxQueued || receivedBytes + bytes > MaxBytes) {
                    stream->close();
                    return;
                }
                received.enqueue(create(stanza.element()));
                receivedBytes += bytes;
            }
            emit q->readyRead();
        });
        connect(stream, &LinkLocalStream::error, this, [this, stream](int) {
#ifdef IRIS_HAVE_LINKLOCAL
            if (auto *known = peers.value(stream->peerJid().full()))
                if (known->opening == stream || channels.value(known->jid.full()) == stream)
                    peerFailed(known, stream->errorText());
#endif
        });
        connect(stream, &LinkLocalStream::connectionClosed, this, [this, stream, socket] {
            const auto key = stream->peerJid().full();
            if (channels.value(key) == stream)
                channels.remove(key);
#ifdef IRIS_HAVE_LINKLOCAL
            if (auto *known = peers.value(key); known && known->opening == stream)
                known->opening = nullptr;
#endif
            streams.remove(stream);
            stream->deleteLater();
            socket->deleteLater();
            maybeClosed();
        });
        return stream;
    }
    Stanza create(const QDomElement &element)
    {
        return q->createStanza(document.importNode(element, true).toElement());
    }
};

LinkLocalService::LinkLocalService(QObject *parent) : Stream(parent), d(new Private(this)) { }
LinkLocalService::~LinkLocalService() { delete d; }
bool LinkLocalService::isSupported()
{
#ifdef IRIS_HAVE_LINKLOCAL
    return true;
#else
    return false;
#endif
}
bool LinkLocalService::start(const Jid &identity, const QHostAddress &address, const QString &name, quint16 port)
{
    if (d->running || d->stopping)
        return false;
    d->base = d->local = identity;
    d->address         = address;
    d->displayName     = name;
#ifdef IRIS_HAVE_LINKLOCAL
    if (!identity.isValid() || !identity.isBare() || identity.node().isEmpty() || identity.domain().contains('.')
        || identity.domain().toLatin1() != identity.domain().toUtf8()
        || QJDnsMulticast::serviceName(identity.full(), Service).isEmpty()) {
        d->fail(tr("Invalid serverless user@machine identity."));
        return false;
    }
    d->dns = QJDnsMulticast::create(address, d);
    if (d->dns)
        connect(d->dns, &QJDnsMulticast::shutdownFinished, d, [this] {
            d->dnsDone = true;
            d->maybeClosed();
        });
    if (!d->dns || !d->server.listen(address, port)) {
        d->fail(tr("Cannot start serverless messaging on the selected network interface or port."));
        return false;
    }
    d->running = true;
    d->browse  = d->dns->subscribe(Browse, QJDns::Ptr);
    connect(d->browse, &QJDnsSharedRequest::resultsReady, d, [this] {
        if (d->stopping)
            return;
        if (!d->browse->success()) {
            d->fail(tr("Serverless discovery failed."));
            return;
        }
        for (const auto &record : d->browse->results())
            d->discover(record);
    });
    QTimer::singleShot(0, d, [this] { d->announce(); });
    return true;
#else
    Q_UNUSED(port)
    d->fail(tr("This Iris build does not include JDNS serverless discovery."));
    return false;
#endif
}
bool LinkLocalService::isOnline() const { return d->announced && !d->stopping; }
Jid  LinkLocalService::localJid() const { return d->local; }
void LinkLocalService::setStatus(const Status &status)
{
    d->status = status;
    if (!status.isAvailable() || status.isInvisible()) {
        close();
        return;
    }
#ifdef IRIS_HAVE_LINKLOCAL
    if (d->txtPublication && !d->stopping)
        d->txtPublication->publishUpdate(d->txtRecord());
#endif
}
void LinkLocalService::setDisplayName(const QString &name)
{
    d->displayName = name;
#ifdef IRIS_HAVE_LINKLOCAL
    if (d->txtPublication && !d->stopping)
        d->txtPublication->publishUpdate(d->txtRecord());
#endif
}
void LinkLocalService::clearSendQueue()
{
#ifdef IRIS_HAVE_LINKLOCAL
    for (auto *peer : d->peers)
        d->clearPeerQueue(peer);
#endif
}
QDomDocument &LinkLocalService::doc() const { return d->document; }
QString       LinkLocalService::baseNS() const { return QStringLiteral("jabber:client"); }
bool          LinkLocalService::old() const { return false; }
void          LinkLocalService::close()
{
    if (d->stopping)
        return;
    d->stopping = true;
    d->server.close();
    clearSendQueue();
#ifdef IRIS_HAVE_LINKLOCAL
    for (auto *peer : d->peers) {
        delete peer->resolution;
        peer->resolution = nullptr;
        emit peerChanged(peer->jid, peer->name, Status(Status::Offline));
    }
    if (d->browse)
        d->browse->cancel();
    if (d->dns)
        d->dns->shutdown();
    else
        d->dnsDone = true;
#else
    d->dnsDone = true;
#endif
    const auto streams = d->streams;
    for (auto *stream : streams)
        stream->close();
    QTimer::singleShot(0, d, [this] { d->maybeClosed(); });
    QTimer::singleShot(6000, d, [this] {
        if (!d->closed) {
            d->closed = true;
            emit connectionClosed();
        }
    });
}
bool   LinkLocalService::stanzaAvailable() const { return !d->received.isEmpty(); }
Stanza LinkLocalService::read()
{
    if (d->received.isEmpty())
        return {};
    auto stanza = d->received.dequeue();
    d->receivedBytes -= stanza.toString().toUtf8().size();
    return stanza;
}
void LinkLocalService::write(const Stanza &stanza)
{
#ifdef IRIS_HAVE_LINKLOCAL
    if (!isOnline() || stanza.isNull() || stanza.kind() == Stanza::Presence)
        return;
    const auto key  = stanza.to().bare();
    auto      *peer = d->peers.value(key);
    if (!peer) {
        emit peerError(stanza.to(), tr("This user is no longer visible on the local network."));
        return;
    }
    auto element = d->document.importNode(stanza.element(), true).toElement();
    element.setAttribute(QStringLiteral("to"), key);
    element.setAttribute(QStringLiteral("from"), d->local.full());
    const auto outgoing = createStanza(element);
    if (auto channel = d->channels.value(key)) {
        if (channel->isConnected()) {
            channel->write(outgoing);
            return;
        }
        d->channels.remove(key);
    }
    const int bytes = outgoing.toString().toUtf8().size();
    if (d->queuedCount >= MaxQueued || d->queuedBytes + bytes > MaxBytes || d->streams.size() >= MaxConnections) {
        emit peerError(peer->jid, tr("Too many pending serverless messages or connections."));
        return;
    }
    peer->pending.enqueue(outgoing);
    d->queuedBytes += bytes;
    ++d->queuedCount;
    d->resolve(peer);
#else
    Q_UNUSED(stanza)
#endif
}
int                     LinkLocalService::errorCondition() const { return Stream::GenericStreamError; }
QString                 LinkLocalService::errorText() const { return d->failure; }
QHash<QString, QString> LinkLocalService::errorLangText() const { return {}; }
QDomElement             LinkLocalService::errorAppSpec() const { return {}; }
}
