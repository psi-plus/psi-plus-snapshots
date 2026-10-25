// SPDX-License-Identifier: LGPL-2.1-or-later
// Server discovery through proxies: RemoteNameProvider (JDNS over DNS over
// TCP), the netnames provider routing and AdvancedConnector's proxied SRV
// connection.
#include <iris/httpconnect.h>
#include <iris/irisnetplugin.h>
#include <iris/netnames.h>
#include <iris/remotenameprovider.h>
#include <iris/xmpp.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHash>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtCrypto>

#include <functional>
#include <memory>

using namespace XMPP;

namespace {
void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

bool waitFor(const std::function<bool()> &done, int timeout = 5000)
{
    QDeadlineTimer deadline(timeout);
    while (!done()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    return true;
}

void appendUInt16(QByteArray &out, quint16 value)
{
    out.append(char(value >> 8));
    out.append(char(value & 0xff));
}

quint16 readUInt16(const QByteArray &data, int at) { return quint16((quint8(data[at]) << 8) | quint8(data[at + 1])); }

// A DNS over TCP server with a fixed zone. Its encoder is independent from
// JDNS and compresses names (RFC 1035 4.1.4).
class FakeDnsServer : public QObject {
public:
    struct Record {
        QByteArray owner;
        quint16    type;
        QByteArray data; // rdata without names, or the prefix before `name`
        QByteArray name; // compressed name ending the rdata (CNAME, SRV target)
    };

    QHash<QPair<QByteArray, quint16>, QList<Record>> zone;
    QList<QByteArray>                                queries; // "name/type"
    QList<quint16>                                   ids;     // message IDs of the queries
    int                                              connections = 0;
    int                                              open        = 0;
    bool dropFirstQuery   = false; // close the first connection instead of answering
    bool ignoreFirstQuery = false; // leave the first request in flight on the same connection

    FakeDnsServer()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto socket = server.nextPendingConnection()) {
                ++connections;
                ++open;
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                    *buffer += socket->readAll();
                    while (buffer->size() >= 2 && buffer->size() >= 2 + readUInt16(*buffer, 0)) {
                        const QByteArray query = buffer->mid(2, readUInt16(*buffer, 0));
                        buffer->remove(0, 2 + query.size());
                        if (dropFirstQuery) {
                            dropFirstQuery = false;
                            socket->disconnectFromHost();
                            return;
                        }
                        const QByteArray reply = answer(query);
                        if (ignoreFirstQuery) {
                            ignoreFirstQuery = false;
                            continue; // backend stays alive but sends no reply
                        }
                        QByteArray frame;
                        appendUInt16(frame, quint16(reply.size()));
                        socket->write(frame + reply);
                    }
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    --open;
                    socket->deleteLater();
                });
            }
        });
    }

    quint16 port() const { return server.serverPort(); }

    void addAddress(const QByteArray &owner, const QHostAddress &address)
    {
        Record record { owner, quint16(address.protocol() == QAbstractSocket::IPv6Protocol ? 28 : 1), {}, {} };
        if (record.type == 1) {
            const quint32 ip = address.toIPv4Address();
            appendUInt16(record.data, quint16(ip >> 16));
            appendUInt16(record.data, quint16(ip));
        } else {
            const Q_IPV6ADDR ip = address.toIPv6Address();
            record.data         = QByteArray(reinterpret_cast<const char *>(ip.c), 16);
        }
        zone[{ owner, record.type }] += record;
    }

    void addSrv(const QByteArray &owner, quint16 priority, quint16 weight, quint16 port, const QByteArray &target)
    {
        Record record { owner, 33, {}, target };
        appendUInt16(record.data, priority);
        appendUInt16(record.data, weight);
        appendUInt16(record.data, port);
        zone[{ owner, 33 }] += record;
    }

    // `owner` is an alias of `target`; address queries follow it.
    void addCname(const QByteArray &owner, const QByteArray &target) { cnames.insert(owner, target); }

private:
    QTcpServer                    server;
    QHash<QByteArray, QByteArray> cnames;

    static QByteArray questionName(const QByteArray &query, int *end)
    {
        QByteArray name;
        int        at = 12;
        while (const int length = quint8(query[at])) {
            if (!name.isEmpty())
                name += '.';
            name += query.mid(at + 1, length);
            at += 1 + length;
        }
        *end = at + 1;
        return name;
    }

    // Writes `name`, pointing at a suffix written before where possible.
    static void writeName(QByteArray &message, QHash<QByteArray, int> &written, const QByteArray &name)
    {
        QByteArray rest = name;
        while (!rest.isEmpty()) {
            if (written.contains(rest)) {
                appendUInt16(message, quint16(0xc000 | written.value(rest)));
                return;
            }
            written.insert(rest, int(message.size()));
            const int        dot   = rest.indexOf('.');
            const QByteArray label = dot < 0 ? rest : rest.left(dot);
            message.append(char(label.size()));
            message.append(label);
            rest = dot < 0 ? QByteArray() : rest.mid(dot + 1);
        }
        message.append('\0');
    }

    QByteArray answer(const QByteArray &query)
    {
        int              questionEnd = 0;
        const QByteArray name        = questionName(query, &questionEnd);
        const quint16    type        = readUInt16(query, questionEnd);
        queries += name + '/' + QByteArray::number(type);
        ids += readUInt16(query, 0);

        QList<Record> answers;
        QByteArray    lookup = name;
        while (cnames.contains(lookup) && type != 5) {
            answers += Record { lookup, 5, {}, cnames.value(lookup) };
            lookup = cnames.value(lookup);
        }
        answers += zone.value({ lookup, type });
        const bool known = answers.size() > 0 || zone.contains({ lookup, 1 }) || zone.contains({ lookup, 28 });

        QByteArray message;
        appendUInt16(message, readUInt16(query, 0));
        appendUInt16(message, quint16(0x8180 | (known ? 0 : 3))); // QR RD RA, NXDOMAIN for unknown names
        appendUInt16(message, 1);
        appendUInt16(message, quint16(answers.size()));
        appendUInt16(message, 0);
        appendUInt16(message, 0);
        QHash<QByteArray, int> written;
        writeName(message, written, name);
        appendUInt16(message, type);
        appendUInt16(message, 1);
        for (const Record &record : std::as_const(answers)) {
            writeName(message, written, record.owner);
            appendUInt16(message, record.type);
            appendUInt16(message, 1);
            appendUInt16(message, 0);
            appendUInt16(message, 300);
            QByteArray data = record.data;
            const int  at   = int(message.size()) + 2;
            if (!record.name.isEmpty()) {
                // names inside rdata are compressed against the whole message
                QByteArray             tail  = message + QByteArray(2, '\0') + data;
                QHash<QByteArray, int> names = written;
                writeName(tail, names, record.name);
                data    = tail.mid(at);
                written = names;
            }
            appendUInt16(message, quint16(data.size()));
            message += data;
        }
        return message;
    }
};

// A SOCKS5 (RFC 1928/1929) or HTTP CONNECT proxy. Host names are resolved
// from its own table only, so a successful lookup through it proves that the
// name was handed to the proxy instead of being resolved locally.
class FakeProxy : public QObject {
public:
    enum class Kind { Socks5, HttpConnect };

    QHash<QByteArray, QHostAddress> hosts;
    QHash<quint16, quint16>         ports;          // requested port -> port connected to, e.g. 5222 -> a test server
    QByteArray                      user, password; // SOCKS5 credentials, empty for none
    QList<QByteArray>               targets;        // "host:port" as requested, IPv6 in brackets

    explicit FakeProxy(Kind kind) : kind(kind)
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto client = server.nextPendingConnection())
                serve(client);
        });
    }

    quint16 port() const { return server.serverPort(); }

private:
    static constexpr int Tunnel = 3; // request handled; the connection relays data

    Kind       kind;
    QTcpServer server;

    void serve(QTcpSocket *client)
    {
        auto buffer = std::make_shared<QByteArray>();
        auto stage  = std::make_shared<int>(0);
        connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        connect(client, &QTcpSocket::readyRead, this, [this, client, buffer, stage]() {
            if (*stage == Tunnel)
                return; // relayed by open()
            *buffer += client->readAll();
            if (kind == Kind::HttpConnect)
                httpRequest(client, buffer, stage);
            else
                socksRequest(client, buffer, stage);
        });
    }

    void httpRequest(QTcpSocket *client, const std::shared_ptr<QByteArray> &buffer, const std::shared_ptr<int> &stage)
    {
        if (!buffer->contains("\r\n\r\n"))
            return;
        *stage                = Tunnel;
        const QByteArray line = buffer->left(buffer->indexOf("\r\n"));
        buffer->clear();
        const QByteArray authority = line.split(' ').value(1);
        const int        colon     = authority.lastIndexOf(':');
        QByteArray       host      = authority.left(colon);
        if (host.startsWith('['))
            host = host.mid(1, host.size() - 2);
        open(client, authority, host, quint16(authority.mid(colon + 1).toInt()), [client](bool ok) {
            client->write(ok ? "HTTP/1.0 200 Connection established\r\n\r\n"
                             : "HTTP/1.0 503 Service Unavailable\r\n\r\n");
        });
    }

    void socksRequest(QTcpSocket *client, const std::shared_ptr<QByteArray> &buffer, const std::shared_ptr<int> &stage)
    {
        QByteArray &b = *buffer;
        if (*stage == 0) { // greeting
            if (b.size() < 2 || b.size() < 2 + quint8(b[1]))
                return;
            const QByteArray methods = b.mid(2, quint8(b[1]));
            b.remove(0, 2 + methods.size());
            const char method = user.isEmpty() ? '\x00' : '\x02';
            if (!methods.contains(method)) {
                client->write(QByteArray("\x05\xff", 2));
                client->disconnectFromHost();
                return;
            }
            client->write(QByteArray("\x05", 1) + method);
            *stage = user.isEmpty() ? 2 : 1;
        }
        if (*stage == 1) { // RFC 1929 username/password
            if (b.size() < 2 || b.size() < 3 + quint8(b[1])
                || b.size() < 3 + quint8(b[1]) + quint8(b[2 + quint8(b[1])]))
                return;
            const QByteArray name = b.mid(2, quint8(b[1]));
            const QByteArray pass = b.mid(3 + name.size(), quint8(b[2 + name.size()]));
            b.remove(0, 3 + name.size() + pass.size());
            const bool ok = name == user && pass == password;
            client->write(QByteArray("\x01", 1) + (ok ? '\x00' : '\x01'));
            if (!ok) {
                client->disconnectFromHost();
                return;
            }
            *stage = 2;
        }
        if (*stage == 2) { // CONNECT
            if (b.size() < 5)
                return;
            QByteArray host;
            int        end = 0;
            if (b.at(3) == 1 && b.size() >= 10) {
                host = QHostAddress(quint32(readUInt16(b, 4)) << 16 | readUInt16(b, 6)).toString().toLatin1();
                end  = 8;
            } else if (b.at(3) == 3 && b.size() >= 7 + quint8(b[4])) {
                host = b.mid(5, quint8(b[4]));
                end  = 5 + host.size();
            } else if (b.at(3) == 4 && b.size() >= 22) {
                host = QHostAddress(reinterpret_cast<const quint8 *>(b.constData() + 4)).toString().toLatin1();
                end  = 20;
            } else {
                return;
            }
            const quint16 port = readUInt16(b, end);
            b.remove(0, end + 2);
            *stage = Tunnel;
            const QByteArray authority
                = (host.contains(':') ? '[' + host + ']' : host) + ':' + QByteArray::number(port);
            open(client, authority, host, port, [client](bool ok) {
                QByteArray reply("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
                if (!ok)
                    reply[1] = '\x05'; // connection refused
                client->write(reply);
            });
        }
    }

    // Connects to the target and relays both directions.
    void open(QTcpSocket *client, const QByteArray &authority, const QByteArray &host, quint16 port,
              const std::function<void(bool)> &reply)
    {
        targets += authority;
        QHostAddress address(QString::fromLatin1(host));
        if (address.isNull())
            address = hosts.value(host);
        if (address.isNull()) {
            reply(false);
            client->disconnectFromHost();
            return;
        }
        auto upstream = new QTcpSocket(client);
        connect(upstream, &QTcpSocket::connected, client, [client, upstream, reply]() {
            reply(true);
            connect(client, &QTcpSocket::readyRead, upstream,
                    [client, upstream]() { upstream->write(client->readAll()); });
            connect(upstream, &QTcpSocket::readyRead, client,
                    [client, upstream]() { client->write(upstream->readAll()); });
            connect(upstream, &QTcpSocket::disconnected, client, &QTcpSocket::disconnectFromHost);
            connect(client, &QTcpSocket::disconnected, upstream, &QTcpSocket::disconnectFromHost);
        });
        connect(upstream, &QTcpSocket::errorOccurred, client, [client, reply](QAbstractSocket::SocketError) {
            if (client->state() == QAbstractSocket::ConnectedState) {
                reply(false);
                client->disconnectFromHost();
            }
        });
        upstream->connectToHost(address, ports.value(port, port));
    }
};

// Answers from a table like the system resolver, without JDNS or a network.
class TableNameProvider : public NameProvider {
public:
    QHash<QPair<QByteArray, int>, QList<NameRecord>> records; // by name and DNS type
    QList<QByteArray>                                queries; // "name/type"

    bool supportsSingle() const override { return true; }

    int resolve_start(const QByteArray &name, int qType, bool) override
    {
        const int id = nextId++;
        queries += name + '/' + QByteArray::number(qType);
        const QList<NameRecord> found = records.value({ name, qType });
        QMetaObject::invokeMethod(
            this,
            [this, id, found]() {
                if (found.isEmpty())
                    emit resolve_error(id, NameResolver::ErrorNoName);
                else
                    emit resolve_resultsReady(id, found);
            },
            Qt::QueuedConnection);
        return id;
    }

    void resolve_stop(int) override { }

private:
    int nextId = 0;
};

// Accepts connections like an XMPP server would; the connector needs no more.
class FakeXmppServer : public QObject {
public:
    int connections = 0;

    FakeXmppServer()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (auto socket = server.nextPendingConnection()) {
                ++connections;
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    quint16 port() const { return server.serverPort(); }

private:
    QTcpServer server;
};

quint16 closedPort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost);
    return probe.serverPort(); // nothing listens once the probe is gone
}

struct Lookup {
    NameResolver      resolver;
    QList<NameRecord> records;
    int               error = -1;
    bool              done  = false;

    Lookup(NameProvider *provider, const QByteArray &name, NameRecord::Type type)
    {
        resolver.setNameProvider(provider);
        QObject::connect(&resolver, &NameResolver::resultsReady, &resolver, [this](const QList<NameRecord> &r) {
            records = r;
            done    = true;
        });
        QObject::connect(&resolver, &NameResolver::error, &resolver, [this](NameResolver::Error e) {
            error = e;
            done  = true;
        });
        resolver.start(name, type);
    }
};

void populate(FakeDnsServer &dns, quint16 deadPort, quint16 xmppPort)
{
    dns.addSrv("_xmpp-client._tcp.example.test", 0, 0, deadPort, "down.example.test");
    dns.addSrv("_xmpp-client._tcp.example.test", 10, 0, xmppPort, "chat.example.test");
    dns.addAddress("down.example.test", QHostAddress::LocalHost);
    dns.addCname("chat.example.test", "xmpp.example.test");
    dns.addAddress("xmpp.example.test", QHostAddress::LocalHost);
    dns.addAddress("xmpp.example.test", QHostAddress::LocalHostIPv6);
    dns.addSrv("_xmpps-client._tcp.tls.test", 0, 0, xmppPort, "xmpp.example.test");
}

void testDnsOverTcp()
{
    FakeDnsServer dns;
    populate(dns, 1, 5222);
    RemoteNameProvider provider(QUrl(QStringLiteral("tcp://127.0.0.1:%1").arg(dns.port())));

    // Concurrent lookups share one pipelined connection.
    Lookup srv(&provider, "_xmpp-client._tcp.example.test", NameRecord::Srv);
    Lookup address(&provider, "chat.example.test.", NameRecord::A);
    Lookup missing(&provider, "_xmpps-client._tcp.example.test", NameRecord::Srv);
    Lookup nodata(&provider, "down.example.test", NameRecord::Aaaa);
    check(waitFor([&]() { return srv.done && address.done && missing.done && nodata.done; }), "lookups did not finish");
    check(dns.connections == 1, "lookups were not pipelined on one connection");

    check(srv.records.size() == 2, "SRV records missing");
    const NameRecord down = srv.records[0].priority() == 0 ? srv.records[0] : srv.records[1];
    const NameRecord chat = srv.records[0].priority() == 0 ? srv.records[1] : srv.records[0];
    check(down.type() == NameRecord::Srv && down.name() == "down.example.test" && down.port() == 1,
          "compressed SRV target misread");
    check(chat.name() == "chat.example.test" && chat.port() == 5222 && chat.priority() == 10, "second SRV misread");

    check(address.records.size() == 1 && address.records[0].address() == QHostAddress(QHostAddress::LocalHost),
          "CNAME chain was not followed to the address");
    check(address.records[0].owner() == QLatin1String("chat.example.test"), "address owner is not the asked name");
    check(missing.error == NameResolver::ErrorNoName, "NXDOMAIN is not ErrorNoName");
    check(nodata.error >= 0 && nodata.records.isEmpty(), "an empty answer did not fail");

    // Literal addresses need no server.
    Lookup literal(&provider, "192.0.2.1", NameRecord::A);
    check(waitFor([&]() { return literal.done; }), "literal lookup did not finish");
    check(literal.records.size() == 1 && literal.records[0].address() == QHostAddress(QStringLiteral("192.0.2.1")),
          "literal address not returned");
    check(dns.queries.size() == 4, "a literal address was sent to the DNS server");

    // A name that cannot be encoded fails without reaching the server.
    Lookup invalid(&provider, QByteArray(64, 'a') + ".example.test", NameRecord::A);
    check(waitFor([&]() { return invalid.done; }), "lookup of an invalid name did not finish");
    check(invalid.error >= 0 && dns.queries.size() == 4, "an invalid name was not failed locally");
}

void testCancelledLookupDoesNotBlockFollowingQueries()
{
    // Native JDNS cancels an external request after the final consumer
    // stops. This does NOT imply the DNS server timed out or went offline.
    FakeDnsServer dns;
    dns.ignoreFirstQuery = true;
    dns.addAddress("abandoned.example.test", QHostAddress::LocalHost);
    dns.addAddress("still-working.example.test", QHostAddress::LocalHost);

    RemoteNameProvider provider(QUrl(QStringLiteral("tcp://127.0.0.1:%1").arg(dns.port())));
    Lookup             abandoned(&provider, "abandoned.example.test", NameRecord::A);
    check(waitFor([&]() { return dns.queries.size() == 1; }), "the first in-flight DNS lookup was not dispatched");
    abandoned.resolver.stop();

    Lookup fresh(&provider, "still-working.example.test", NameRecord::A);
    check(waitFor([&]() { return fresh.done; }), "the DNS lookup after cancellation did not finish");
    check(fresh.records.size() == 1 && fresh.records.first().address() == QHostAddress::LocalHost,
          "cancelling a DNS lookup incorrectly marked a live DNS server unavailable");
    check(!abandoned.done, "a cancelled DNS lookup emitted a late result");
    check(dns.queries.size() == 2, "the second DNS query was not sent upstream");
}

void testReconnectsOnce()
{
    // A server may close a connection with queries in flight (RFC 7766
    // 6.2.3): they are sent again, with fresh IDs, on a new connection.
    FakeDnsServer dns;
    populate(dns, 1, 5222);
    dns.dropFirstQuery = true;
    RemoteNameProvider provider(QUrl(QStringLiteral("tcp://127.0.0.1:%1").arg(dns.port())));
    Lookup             lookup(&provider, "xmpp.example.test", NameRecord::A);
    check(waitFor([&]() { return lookup.done; }), "lookup did not finish after the connection was dropped");
    check(lookup.records.size() == 1, "the query was not sent again");
    check(dns.connections == 2, "the query was not sent on a new connection");

    // IDs count up on each connection, starting from zero.
    Lookup next(&provider, "down.example.test", NameRecord::A);
    check(waitFor([&]() { return next.done; }), "lookup on the new connection did not finish");
    check(dns.ids == QList<quint16> { 0, 1 }, "message IDs are not per connection");
}

void testServiceResolverProtocolFallback()
{
    // An IPv6-only host: the A lookup finds nothing, the AAAA fallback must
    // deliver its address (it used to be connected to a missing slot).
    TableNameProvider provider;
    NameRecord        address(QByteArray("v6only.example.test"), 300);
    address.setAddress(QHostAddress(QHostAddress::LocalHostIPv6));
    provider.records.insert({ "v6only.example.test", 28 }, { address });
    ServiceResolver resolver;
    resolver.setNameProvider(&provider);
    resolver.setProtocol(ServiceResolver::IPv4_IPv6);
    QHostAddress found;
    QString      host;
    bool         failed = false;
    QObject::connect(&resolver, &ServiceResolver::resultReady, &resolver,
                     [&](const QHostAddress &address, quint16 port, const QString &hostname, const QString &) {
                         check(port == 5222, "fallback lost the port");
                         found = address;
                         host  = hostname;
                     });
    QObject::connect(&resolver, &ServiceResolver::error, &resolver, [&]() { failed = true; });
    resolver.start(QStringLiteral("v6only.example.test"), 5222);
    check(waitFor([&]() { return !found.isNull() || failed; }), "address fallback result was not delivered");
    check(found == QHostAddress(QHostAddress::LocalHostIPv6) && host == QLatin1String("v6only.example.test"),
          "address fallback returned a wrong address");
    check(provider.queries == QList<QByteArray> { "v6only.example.test/1", "v6only.example.test/28" },
          "address fallback did not ask for A, then AAAA");
}

void testThroughProxies()
{
    for (const auto kind : { FakeProxy::Kind::Socks5, FakeProxy::Kind::HttpConnect }) {
        FakeDnsServer dns;
        populate(dns, 1, 5222);
        FakeProxy proxy(kind);
        proxy.hosts.insert("dns.test", QHostAddress(QHostAddress::LocalHost));
        const QNetworkProxy::ProxyType type
            = kind == FakeProxy::Kind::Socks5 ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy;

        // dns.test resolves on the proxy only: success proves it was not resolved locally
        RemoteNameProvider provider(QUrl(QStringLiteral("tcp://dns.test:%1").arg(dns.port())),
                                    QNetworkProxy(type, QStringLiteral("127.0.0.1"), proxy.port()));
        Lookup             lookup(&provider, "xmpp.example.test", NameRecord::A);
        check(waitFor([&]() { return lookup.done; }), "proxied lookup did not finish");
        check(lookup.records.size() == 1, "proxied lookup failed");
        check(proxy.targets == QList<QByteArray> { "dns.test:" + QByteArray::number(dns.port()) },
              "the DNS server was not reached through the proxy by name");
    }
}

void testConnectorResolvesThroughProxy()
{
    FakeXmppServer xmpp;
    FakeDnsServer  dns;
    const quint16  deadPort = closedPort();
    populate(dns, deadPort, xmpp.port());
    FakeProxy proxy(FakeProxy::Kind::Socks5);
    proxy.hosts.insert("dns.test", QHostAddress(QHostAddress::LocalHost));

    AdvancedConnector        connector;
    AdvancedConnector::Proxy settings;
    settings.setSocks(QStringLiteral("127.0.0.1"), proxy.port());
    connector.setProxy(settings);
    connector.setRemoteDns(QUrl(QStringLiteral("tcp://dns.test:%1").arg(dns.port())));
    bool connected = false, failed = false;
    QObject::connect(&connector, &Connector::connected, &connector, [&]() { connected = true; });
    QObject::connect(&connector, &Connector::error, &connector, [&]() { failed = true; });
    connector.connectToServer(QStringLiteral("example.test")); // unknown to the local resolver

    check(waitFor([&]() { return connected || failed; }), "connector did not finish");
    check(connected, "connector failed to connect through the proxy");
    check(xmpp.connections == 1, "the XMPP server was not reached");
    // SRV order: the unreachable target first, then the next one by address.
    const QList<QByteArray> expectedTargets { "dns.test:" + QByteArray::number(dns.port()),
                                              "127.0.0.1:" + QByteArray::number(deadPort),
                                              "127.0.0.1:" + QByteArray::number(xmpp.port()) };
    check(proxy.targets == expectedTargets, "the proxy was not given the resolved addresses in SRV order");
    check(dns.queries.contains("_xmpps-client._tcp.example.test/33")
              && dns.queries.contains("_xmpp-client._tcp.example.test/33")
              && dns.queries.contains("down.example.test/1") && dns.queries.contains("chat.example.test/1"),
          "SRV and address lookups did not go to the remote DNS server");
    check(connector.host() == QLatin1String("chat.example.test"), "host is not the SRV target");
    check(!connector.useSSL(), "plain xmpp-client target switched to direct TLS");
    check(waitFor([&]() { return dns.open == 0; }), "the DNS connection stayed open after connecting");

    // xmpps-client targets use direct TLS
    AdvancedConnector tls;
    tls.setProxy(settings);
    tls.setRemoteDns(QUrl(QStringLiteral("tcp://dns.test:%1").arg(dns.port())));
    bool tlsConnected = false;
    QObject::connect(&tls, &Connector::connected, &tls, [&]() { tlsConnected = true; });
    tls.connectToServer(QStringLiteral("tls.test"));
    check(waitFor([&]() { return tlsConnected; }), "xmpps-client target was not connected");
    check(tls.useSSL(), "xmpps-client target did not request direct TLS");
}

void testConnectorReportsProxyErrors()
{
    FakeDnsServer dns;
    populate(dns, 1, 5222);
    FakeProxy proxy(FakeProxy::Kind::Socks5);
    proxy.hosts.insert("dns.test", QHostAddress(QHostAddress::LocalHost));
    proxy.user     = "alice";
    proxy.password = "secret";

    AdvancedConnector        connector;
    AdvancedConnector::Proxy settings;
    settings.setSocks(QStringLiteral("127.0.0.1"), proxy.port());
    settings.setUserPass(QStringLiteral("alice"), QStringLiteral("wrong"));
    connector.setProxy(settings);
    connector.setRemoteDns(QUrl(QStringLiteral("tcp://dns.test:%1").arg(dns.port())));
    bool failed = false;
    QObject::connect(&connector, &Connector::error, &connector, [&]() { failed = true; });
    connector.connectToServer(QStringLiteral("example.test"));
    check(waitFor([&]() { return failed; }), "connector did not fail");
    check(connector.errorCode() == AdvancedConnector::ErrProxyAuth, "proxy authentication failure was not reported");
    check(dns.queries.isEmpty(), "DNS was queried without passing the proxy");
}

void testNoSystemFallback()
{
    // A resolver bound to a provider must not fall back to the system
    // resolver once the provider is gone.
    auto   provider = std::make_unique<RemoteNameProvider>(QUrl(QStringLiteral("tcp://127.0.0.1:1")));
    Lookup pending(provider.get(), "localhost", NameRecord::A);
    provider.reset();
    check(waitFor([&]() { return pending.done; }), "lookup of a deleted provider did not finish");
    check(pending.error == NameResolver::ErrorGeneric && pending.records.isEmpty(),
          "lookup of a deleted provider was not failed");

    NameResolver later;
    int          laterError = -1;
    {
        RemoteNameProvider gone(QUrl(QStringLiteral("tcp://127.0.0.1:1")));
        later.setNameProvider(&gone);
    }
    QObject::connect(&later, &NameResolver::resultsReady, &later, [](const QList<NameRecord> &) {
        check(false, "a deleted provider fell back to the system resolver");
    });
    QObject::connect(&later, &NameResolver::error, &later, [&](NameResolver::Error e) { laterError = e; });
    later.start("localhost", NameRecord::A);
    check(waitFor([&]() { return laterError >= 0; }), "lookup after the provider was deleted did not finish");
    check(laterError == NameResolver::ErrorGeneric, "lookup after the provider was deleted was not failed");

    bool            serviceFailed = false;
    ServiceResolver service;
    {
        RemoteNameProvider gone(QUrl(QStringLiteral("tcp://127.0.0.1:1")));
        service.setNameProvider(&gone);
    }
    QObject::connect(&service, &ServiceResolver::resultReady, &service,
                     [](const QHostAddress &, quint16, const QString &, const QString &) {
                         check(false, "service resolver used the system resolver");
                     });
    QObject::connect(&service, &ServiceResolver::error, &service, [&]() { serviceFailed = true; });
    service.start(QStringLiteral("localhost"), 5222);
    check(waitFor([&]() { return serviceFailed; }), "service lookup after the provider was deleted did not fail");
}

void testWithoutJdns()
{
    // Nothing is resolved, and nothing falls back to the system resolver.
    RemoteNameProvider provider(QUrl(QStringLiteral("tcp://127.0.0.1:1")));
    Lookup             lookup(&provider, "localhost", NameRecord::A);
    check(waitFor([&]() { return lookup.done; }), "lookup without JDNS did not finish");
    check(lookup.error == NameResolver::ErrorGeneric && lookup.records.isEmpty(), "lookup without JDNS did not fail");

    // The connector ignores remote DNS: the system resolver finds no SRV
    // records for the .invalid domain (RFC 6761), so the proxy gets the domain.
    FakeProxy                proxy(FakeProxy::Kind::Socks5);
    AdvancedConnector        connector;
    AdvancedConnector::Proxy settings;
    settings.setSocks(QStringLiteral("127.0.0.1"), proxy.port());
    connector.setProxy(settings);
    connector.setRemoteDns(QUrl(QStringLiteral("tcp://127.0.0.1:1")));
    bool failed = false;
    QObject::connect(&connector, &Connector::error, &connector, [&]() { failed = true; });
    connector.connectToServer(QStringLiteral("example.invalid"));
    check(waitFor([&]() { return failed; }), "connector did not finish");
    check(proxy.targets == QList<QByteArray> { "example.invalid:5222" }, "the proxy was not given the domain");
}

void testServiceResolverTargetsByName()
{
    // For a proxy that resolves names: SRV targets in order, then the domain,
    // all by name; no address is looked up.
    TableNameProvider provider;
    NameRecord        backup(QByteArray("_xmpp-client._tcp.example.test."), 300);
    backup.setSrv("backup.example.test", 5269, 10, 0);
    NameRecord primary(QByteArray("_xmpp-client._tcp.example.test."), 300);
    primary.setSrv("primary.example.test", 5222, 0, 0);
    provider.records.insert({ "_xmpp-client._tcp.example.test.", 33 }, { backup, primary });

    ServiceResolver resolver;
    resolver.setNameProvider(&provider);
    resolver.setResolveTargets(false);
    QList<QByteArray> targets;
    bool              done = false;
    QObject::connect(&resolver, &ServiceResolver::resultReady, &resolver,
                     [&](const QHostAddress &address, quint16 port, const QString &host, const QString &service) {
                         check(address.isNull(), "an SRV target was resolved to an address");
                         targets += host.toLatin1() + ':' + QByteArray::number(port) + '/' + service.toLatin1();
                         QMetaObject::invokeMethod(&resolver, [&]() { resolver.tryNext(); }, Qt::QueuedConnection);
                     });
    QObject::connect(&resolver, &ServiceResolver::error, &resolver, [&]() { done = true; });
    resolver.start({ QStringLiteral("xmpps-client"), QStringLiteral("xmpp-client") }, QStringLiteral("tcp"),
                   QStringLiteral("example.test"), 5222);
    check(waitFor([&]() { return done; }), "SRV targets were not all reported");
    check(targets
              == QList<QByteArray> { "primary.example.test:5222/xmpp-client", "backup.example.test:5269/xmpp-client",
                                     "example.test:5222/" },
          "SRV targets were not reported by name in order, followed by the domain");
    check(provider.queries
              == QList<QByteArray> { "_xmpps-client._tcp.example.test./33", "_xmpp-client._tcp.example.test./33" },
          "addresses were looked up although the proxy resolves them");
}

void testConnectorLocalSrvThroughProxy()
{
    // Without remote DNS only SRV records are looked up, with the system
    // resolver; the proxy resolves what it is given. Here SRV finds nothing
    // (.invalid, RFC 6761), so the proxy gets the domain and the default port.
    FakeXmppServer xmpp;
    FakeProxy      proxy(FakeProxy::Kind::HttpConnect);
    proxy.hosts.insert("example.invalid", QHostAddress(QHostAddress::LocalHost));
    proxy.ports.insert(5222, xmpp.port());

    AdvancedConnector        connector;
    AdvancedConnector::Proxy settings;
    settings.setHttpConnect(QStringLiteral("127.0.0.1"), proxy.port());
    connector.setProxy(settings);
    bool connected = false, failed = false;
    QObject::connect(&connector, &Connector::connected, &connector, [&]() { connected = true; });
    QObject::connect(&connector, &Connector::error, &connector, [&]() { failed = true; });
    connector.connectToServer(QStringLiteral("example.invalid"));
    check(waitFor([&]() { return connected || failed; }), "connector did not finish");
    check(connected && proxy.targets == QList<QByteArray> { "example.invalid:5222" },
          "the proxy was not given the domain after SRV found nothing");
    check(connector.host() == QLatin1String("example.invalid"), "host is not the domain");
}

void testConnectorRemoteDnsLastResort()
{
    // The DNS server cannot be reached through the proxy: the proxy is given
    // the domain, as without SRV.
    FakeXmppServer xmpp;
    FakeProxy      proxy(FakeProxy::Kind::Socks5);
    const quint16  deadPort = closedPort();
    proxy.hosts.insert("dns.test", QHostAddress(QHostAddress::LocalHost));
    proxy.hosts.insert("example.test", QHostAddress(QHostAddress::LocalHost));
    proxy.ports.insert(5222, xmpp.port());

    AdvancedConnector        connector;
    AdvancedConnector::Proxy settings;
    settings.setSocks(QStringLiteral("127.0.0.1"), proxy.port());
    connector.setProxy(settings);
    connector.setRemoteDns(QUrl(QStringLiteral("tcp://dns.test:%1").arg(deadPort)));
    bool connected = false, failed = false;
    QObject::connect(&connector, &Connector::connected, &connector, [&]() { connected = true; });
    QObject::connect(&connector, &Connector::error, &connector, [&]() { failed = true; });
    connector.connectToServer(QStringLiteral("example.test"));
    check(waitFor([&]() { return connected || failed; }), "connector did not finish");
    check(connected, "the connector gave up instead of handing the proxy the domain");
    check(proxy.targets == QList<QByteArray> { "dns.test:" + QByteArray::number(deadPort), "example.test:5222" },
          "the proxy was not given the domain after the DNS server failed");
}

void testRemoteDnsEndpointValidation()
{
    check(RemoteNameProvider::isValidServer(QUrl(QStringLiteral("tcp://1.1.1.1"))),
          "DNS over TCP without a port should be accepted");
    check(RemoteNameProvider::isValidServer(QUrl(QStringLiteral("tcp://dns.example:853"))),
          "explicit DNS over TCP port should be accepted");
    check(RemoteNameProvider::isValidServer(QUrl(QStringLiteral("https://dns.example/dns-query?token=abc"))),
          "DoH path and query should be accepted");

    for (const QString &url : {
             QStringLiteral("http://dns.example/dns-query"),
             QStringLiteral("tcp://dns.example/ignored/path"),
             QStringLiteral("tcp://dns.example/?ignored=1"),
             QStringLiteral("https://user:password@dns.example/dns-query"),
             QStringLiteral("https://dns.example/dns-query#fragment"),
             QStringLiteral("tcp://dns.example:0"),
             QStringLiteral("https://dns.example:0/dns-query"),
             QStringLiteral("https:///dns-query"),
         }) {
        check(!RemoteNameProvider::isValidServer(QUrl(url)), "invalid DNS endpoint URL was accepted");
    }
}

void testHttpConnectIpv6Authority()
{
    FakeXmppServer target;
    FakeProxy      proxy(FakeProxy::Kind::HttpConnect);
    HttpConnect    connection;
    bool           done = false;
    QObject::connect(&connection, &HttpConnect::connected, &connection, [&]() { done = true; });
    QObject::connect(&connection, &HttpConnect::error, &connection, [&](int) { done = true; });
    connection.connectToHost(QStringLiteral("127.0.0.1"), proxy.port(), QStringLiteral("::1"), target.port());
    check(waitFor([&]() { return done; }), "HTTP CONNECT did not finish");
    check(proxy.targets == QList<QByteArray> { "[::1]:" + QByteArray::number(target.port()) },
          "IPv6 CONNECT target is not bracketed");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    if (RemoteNameProvider::isSupported()) {
        testDnsOverTcp();
        testCancelledLookupDoesNotBlockFollowingQueries();
        testReconnectsOnce();
        testThroughProxies();
        testConnectorResolvesThroughProxy();
        testConnectorReportsProxyErrors();
        testConnectorRemoteDnsLastResort();
    } else {
        testWithoutJdns();
    }
    testServiceResolverProtocolFallback();
    testServiceResolverTargetsByName();
    testConnectorLocalSrvThroughProxy();
    testRemoteDnsEndpointValidation();
    testNoSystemFallback();
    testHttpConnectIpv6Authority();
    return 0;
}
