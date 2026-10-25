/* Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * XEP-0174 example; application presence/discovery policy stays outside JDNS.
 */
#include <iris/processquit.h>
#include <iris/xmpp_linklocalstream.h>
#include <iris/xmpp_stanza.h>
#include <qjdnsmulticast.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

namespace {
const QByteArray Service("_presence._tcp");
const QByteArray BrowseName("_presence._tcp.local.");
using XMPP::Jid;
using XMPP::LinkLocalStream;
using XMPP::Stanza;

class Peer : public QObject {
public:
    QJDnsMulticast *dns;
    QTcpServer      server;
    Jid             base, local, destination;
    QString         body, expected;
    bool echo, exitAfterMessage, emptyTxt, stopping = false, dnsDone = false, attempted = false, announced = false;
    int  code = 0, hostNumber = 0, userNumber = 0, generation = 0;
    QList<QJDnsSharedRequest *> publications;
    QPointer<QObject>           resolution;
    QSet<LinkLocalStream *>     streams;
    QSet<QString>               known;
    QByteArray                  targetInstance;
    QHostAddress                address;

    Peer(QJDnsMulticast *session, const QHostAddress &bindAddress, const Jid &identity,
         const QCommandLineParser &arguments) :
        dns(session), base(identity), local(identity), destination(arguments.value("send")),
        body(arguments.value("message")), expected(arguments.value("expect")), echo(arguments.isSet("echo")),
        exitAfterMessage(arguments.isSet("exit-after-message")), emptyTxt(arguments.isSet("empty-txt")),
        address(bindAddress)
    {
        dns->setParent(this);
        connect(dns, &QJDnsMulticast::shutdownFinished, this, [this] {
            dnsDone = true;
            qInfo().noquote() << "OFFLINE" << local.full();
            maybeQuit();
        });
        connect(XMPP::ProcessQuit::instance(), &XMPP::ProcessQuit::quit, this, [this] { stop(); });
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                socket->setParent(this);
                attach(socket, {}, LinkLocalStream::Direction::Recipient);
            }
        });
        if (!server.listen(address, 0)) {
            fail(QStringLiteral("Cannot listen on selected interface"));
            return;
        }
        auto *browse = dns->subscribe(BrowseName, QJDns::Ptr);
        connect(browse, &QJDnsSharedRequest::resultsReady, this, [this, browse] {
            if (stopping)
                return;
            if (!browse->success()) {
                fail(QStringLiteral("mDNS browse failed"));
                return;
            }
            for (const auto &record : browse->results()) {
                if (record.type != QJDns::Ptr)
                    continue;
                const Jid peer(QJDnsMulticast::instanceName(record.name, Service));
                if (!peer.isValid() || peer.node().isEmpty() || !peer.isBare() || peer == local)
                    continue;
                if (record.ttl) {
                    if (!known.contains(peer.full())) {
                        known.insert(peer.full());
                        qInfo().noquote() << "AVAILABLE" << peer.full();
                    }
                    if (peer == destination) {
                        targetInstance = record.name;
                        if (announced && !attempted)
                            resolve(record.name, peer);
                    }
                } else {
                    if (known.remove(peer.full()))
                        qInfo().noquote() << "UNAVAILABLE" << peer.full();
                    if (peer == destination && resolution) {
                        delete resolution;
                        attempted = false;
                    }
                }
            }
        });
        announce();
    }

    void fail(const QString &reason)
    {
        qWarning().noquote() << reason;
        code = 1;
        stop();
    }
    void maybeQuit()
    {
        if (stopping && dnsDone && streams.isEmpty())
            QCoreApplication::exit(code);
    }
    void stop()
    {
        if (stopping)
            return;
        stopping = true;
        server.close();
        delete resolution;
        const auto active = streams;
        for (auto *stream : active)
            stream->close();
        dns->shutdown();
        QTimer::singleShot(7000, this, [this] {
            qWarning("Timed out shutting down link-local example");
            QCoreApplication::exit(1);
        });
    }

    void announce()
    {
        if (stopping)
            return;
        ++generation;
        announced        = false;
        const int serial = generation;
        for (auto *publication : publications)
            delete publication; // withdraw old PTR before renaming a host/user
        publications.clear();
        local               = Jid(base.node() + (userNumber ? "-" + QString::number(userNumber) : QString()),
                                  base.domain() + (hostNumber ? "-" + QString::number(hostNumber) : QString()));
        const auto instance = QJDnsMulticast::serviceName(local.full(), Service);
        const auto host     = local.domain().toLatin1() + ".local.";
        if (instance.isEmpty()) {
            fail(QStringLiteral("Presence instance exceeds a DNS label"));
            return;
        }
        QJDns::Record addressRecord;
        addressRecord.owner     = host;
        addressRecord.type      = address.protocol() == QAbstractSocket::IPv6Protocol ? QJDns::Aaaa : QJDns::A;
        addressRecord.haveKnown = true;
        addressRecord.address   = address;
        addressRecord.ttl       = 120;
        QJDns::Record srv;
        srv.owner     = instance;
        srv.type      = QJDns::Srv;
        srv.haveKnown = true;
        srv.name      = host;
        srv.port      = server.serverPort();
        srv.priority = srv.weight = 0;
        srv.ttl                   = 120;
        QJDns::Record txt;
        txt.owner     = instance;
        txt.type      = QJDns::Txt;
        txt.haveKnown = true;
        txt.texts     = { "txtvers=1", "status=avail", "port.p2pj=" + QByteArray::number(server.serverPort()) };
        if (emptyTxt)
            txt.texts.clear();
        txt.ttl        = 4500;
        auto remaining = std::make_shared<int>(3);
        for (const auto &record : { addressRecord, srv, txt }) {
            auto *publication = dns->publish(record);
            publications.append(publication);
            auto confirmed = std::make_shared<bool>(false);
            connect(publication, &QJDnsSharedRequest::resultsReady, this,
                    [this, publication, remaining, confirmed, serial, instance, record, host] {
                        if (serial != generation || stopping)
                            return;
                        if (!publication->success()) {
                            if (publication->error() == QJDnsSharedRequest::ErrorConflict) {
                                if (record.owner == host)
                                    ++hostNumber;
                                else
                                    ++userNumber;
                                ++generation; // retire all callbacks until the queued rename
                                qInfo().noquote() << "RENAMING" << local.full();
                                QTimer::singleShot(0, this, [this] { announce(); });
                            } else
                                fail(QStringLiteral("Cannot publish presence metadata"));
                            return;
                        }
                        if (*confirmed)
                            return;
                        *confirmed = true;
                        if (--*remaining != 0)
                            return;
                        QJDns::Record ptr;
                        ptr.owner          = BrowseName;
                        ptr.type           = QJDns::Ptr;
                        ptr.haveKnown      = true;
                        ptr.name           = instance;
                        ptr.ttl            = 4500;
                        auto *announcement = dns->publish(ptr, QJDns::Shared);
                        publications.prepend(announcement);
                        connect(announcement, &QJDnsSharedRequest::resultsReady, this, [this, announcement, serial] {
                            if (serial != generation || stopping)
                                return;
                            if (announcement->success()) {
                                announced = true;
                                qInfo().noquote() << "ANNOUNCED" << local.full() << "port=" << server.serverPort();
                                if (!targetInstance.isEmpty() && !attempted)
                                    resolve(targetInstance, destination);
                            } else
                                fail(QStringLiteral("Cannot announce presence PTR"));
                        });
                    });
        }
    }

    void resolve(const QByteArray &instance, const Jid &peer)
    {
        attempted     = true;
        auto *context = new QObject(this);
        resolution    = context;
        auto *srv     = dns->subscribe(instance, QJDns::Srv, context);
        // TXT is optional metadata. Its absence/empty value never prevents TCP.
        auto *txt = dns->subscribe(instance, QJDns::Txt, context);
        connect(txt, &QJDnsSharedRequest::resultsReady, context, [txt] { txt->cancel(); });
        connect(srv, &QJDnsSharedRequest::resultsReady, context, [this, srv, context, peer] {
            if (!srv->success()) {
                fail(QStringLiteral("Presence SRV lookup failed"));
                return;
            }
            for (const auto &record : srv->results()) {
                if (!record.ttl || record.type != QJDns::Srv || record.port < 1 || record.port > 65535
                    || !record.name.toLower().endsWith(".local."))
                    continue;
                srv->cancel();
                auto connected = std::make_shared<bool>(false);
                for (const auto type : { QJDns::A, QJDns::Aaaa }) {
                    auto *lookup = dns->subscribe(record.name, type, context);
                    connect(
                        lookup, &QJDnsSharedRequest::resultsReady, context,
                        [this, context, lookup, peer, record, connected] {
                            if (*connected || !lookup->success())
                                return;
                            for (const auto &answer : lookup->results()) {
                                if (!answer.ttl || (answer.type != QJDns::A && answer.type != QJDns::Aaaa)
                                    || answer.address.isNull() || answer.address.isMulticast()
                                    || answer.address.protocol() != address.protocol())
                                    continue;
                                auto remote = answer.address;
                                if (remote.protocol() == QAbstractSocket::IPv6Protocol) {
                                    const auto bytes = remote.toIPv6Address();
                                    if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80)
                                        remote.setScopeId(dns->networkInterface().name());
                                }
                                *connected   = true;
                                auto *socket = new QTcpSocket(this);
                                if (!socket->bind(address, 0)) {
                                    socket->deleteLater();
                                    fail(QStringLiteral("Cannot bind peer socket to selected interface"));
                                    return;
                                }
                                connect(socket, &QTcpSocket::connected, this, [this, socket, peer] {
                                    attach(socket, peer, LinkLocalStream::Direction::Initiator);
                                });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                                connect(socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
#else
                            connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
                                    [this](QAbstractSocket::SocketError) {
#endif
                                    fail(QStringLiteral("Peer TCP connection failed"));
                                });
                                qInfo().noquote() << "CONNECTING" << peer.full() << remote.toString() << record.port;
                                socket->connectToHost(remote, quint16(record.port));
                                // Cancel detail subscriptions now; retain no stale IP cache.
                                context->deleteLater();
                                return;
                            }
                        });
                }
                return;
            }
        });
        QTimer::singleShot(10000, context, [this] { fail(QStringLiteral("Presence lookup timed out")); });
    }

    void attach(QTcpSocket *socket, const Jid &peer, LinkLocalStream::Direction direction)
    {
        if (stopping) {
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }
        auto *stream = new LinkLocalStream(socket, local, peer, direction, this);
        streams.insert(stream);
        connect(stream, &LinkLocalStream::connected, this, [this, stream, direction] {
            qInfo().noquote() << "CONNECTED" << stream->peerJid().full();
            if (direction == LinkLocalStream::Direction::Initiator) {
                auto stanza = stream->createStanza(Stanza::Message, stream->peerJid(), QStringLiteral("chat"));
                stanza.appendChild(stanza.createTextElement(stream->baseNS(), QStringLiteral("body"), body));
                stream->write(stanza);
            }
        });
        connect(stream, &LinkLocalStream::readyRead, this, [this, stream, direction] {
            while (stream->stanzaAvailable()) {
                const auto stanza = stream->read();
                const auto text   = stanza.element().firstChildElement(QStringLiteral("body")).text();
                if (stanza.kind() != Stanza::Message)
                    continue;
                qInfo().noquote() << "RECEIVED" << stream->peerJid().full() << "chars=" << text.size();
                if (!expected.isEmpty() && text != expected) {
                    fail(QStringLiteral("Received body differs from expected body"));
                    return;
                }
                if (echo && direction == LinkLocalStream::Direction::Recipient) {
                    auto reply = stream->createStanza(Stanza::Message, stream->peerJid(), QStringLiteral("chat"));
                    reply.appendChild(reply.createTextElement(stream->baseNS(), QStringLiteral("body"), text));
                    stream->write(reply);
                }
                if (exitAfterMessage)
                    stop();
            }
        });
        connect(stream, &LinkLocalStream::error, this, [this, stream](int) { fail(stream->errorText()); });
        connect(stream, &LinkLocalStream::connectionClosed, this, [this, stream, socket] {
            streams.remove(stream);
            stream->deleteLater();
            socket->deleteLater();
            maybeQuit();
        });
    }
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("iris-linklocal"));
    QCommandLineParser arguments;
    arguments.setApplicationDescription(QStringLiteral("XEP-0174 example over QJDnsMulticast and Iris XML streams"));
    arguments.addHelpOption();
    arguments.addOptions(
        { { { "i", "interface" }, "Network interface name (uses its first IPv4 address, otherwise IPv6)", "name" },
          { "address", "Concrete local source address, including IPv6 scope when needed", "ip" },
          { "jid", "Local user@machine identity (ASCII machine label)", "jid" },
          { "send", "Peer to discover and send one message to", "jid" },
          { "message", "Message body for --send", "text" },
          { "expect", "Fail if a received message body differs from this value", "text" },
          { "echo", "Echo received messages in this test/example" },
          { "empty-txt", "Publish a mandatory empty TXT record, without optional metadata" },
          { "exit-after-message", "Go offline after receiving a message" },
          { "run-for", "Go offline after this many milliseconds", "ms" },
          { "allow-plain", "Explicitly allow this example's unencrypted/unauthenticated TCP streams" } });
    arguments.process(app);
    if (!arguments.isSet("allow-plain")) {
        qCritical("This example requires --allow-plain; TLS/SASL are not implemented.");
        return 2;
    }
    const Jid jid(arguments.value("jid"));
    if (!jid.isValid() || !jid.isBare() || jid.node().isEmpty() || jid.domain().contains('.')
        || jid.domain().toLatin1() != jid.domain().toUtf8()
        || QJDnsMulticast::serviceName(jid.full(), Service).isEmpty()) {
        qCritical("--jid must be user@machine with one ASCII machine label and a 63-octet presence instance");
        return 2;
    }
    if (arguments.isSet("send") && (!Jid(arguments.value("send")).isValid() || !arguments.isSet("message"))) {
        qCritical("--send requires a valid peer identity and --message");
        return 2;
    }
    QHostAddress address(arguments.value("address"));
    if (arguments.isSet("interface") == arguments.isSet("address")) {
        qCritical("Choose exactly one of --interface or --address");
        return 2;
    }
    if (arguments.isSet("interface")) {
        const auto interface = QNetworkInterface::interfaceFromName(arguments.value("interface"));
        for (const auto &entry : interface.addressEntries()) {
            if (address.isNull() || entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol)
                break;
        }
    }
    auto *dns = QJDnsMulticast::create(address);
    if (!dns) {
        qCritical("Cannot create mDNS session on the requested interface/address");
        return 1;
    }
    Peer peer(dns, address, jid, arguments);
    if (arguments.isSet("run-for")) {
        bool      ok       = false;
        const int duration = arguments.value("run-for").toInt(&ok);
        if (!ok || duration <= 0) {
            qCritical("--run-for must be a positive integer");
            return 2;
        }
        QTimer::singleShot(duration, &peer, [&peer] { peer.stop(); });
    }
    return app.exec();
}
