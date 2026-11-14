/*
 * QJDns transport tests: the SRV wire response is parsed by JDNS itself.
 * Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: MIT
 */
#include <qjdns.h>
#include <qjdns_tcptransport.h>
#include <qjdns_httptransport.h>
#include <qjdns_externaltransport.h>
#include <qjdnsshared.h>

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTest>
#include <QTimer>
#include <cstring>
#include <memory>

namespace {
void put16(QByteArray *buf, quint16 v)
{
    buf->append(char(v >> 8));
    buf->append(char(v));
}
quint16 get16(const QByteArray &buf, int pos)
{
    return quint16(quint8(buf.at(pos)) << 8) | quint8(buf.at(pos + 1));
}
QByteArray encodeName(const QByteArray &name)
{
    QByteArray out;
    for(const QByteArray &part : name.split('.'))
    {
        out.append(char(part.size()));
        out.append(part);
    }
    out.append(char(0));
    return out;
}
QByteArray srvAnswer(const QByteArray &request)
{
    QByteArray result = request;
    result[2] = char(0x81); // QR and RD
    result[3] = char(0x80); // RA and NOERROR
    result[6] = 0;
    result[7] = 1; // one answer
    for(int i = 8; i < 12; ++i)
        result[i] = 0;

    // Compressed owner pointer to original question, followed by SRV rdata.
    result.append(char(0xc0));
    result.append(char(0x0c));
    put16(&result, 33); // SRV
    put16(&result, 1); // IN
    result.append(QByteArray::fromHex("0000003c")); // TTL 60
    const QByteArray target = encodeName("chat.example.com");
    put16(&result, quint16(6 + target.size()));
    put16(&result, 1); // priority
    put16(&result, 1); // weight
    put16(&result, 5222);
    result += target;
    return result;
}
class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, const QByteArray &answer, int status, QObject *parent)
        : QNetworkReply(parent), answer_(answer)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::PostOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/dns-message"));
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this]() {
            setFinished(true);
            emit readyRead();
            emit finished();
        });
    }
    void abort() override {}
    qint64 bytesAvailable() const override
    {
        return (answer_.size() - position_) + QNetworkReply::bytesAvailable();
    }
protected:
    qint64 readData(char *dst, qint64 maxSize) override
    {
        const qint64 amount = qMin<qint64>(maxSize, answer_.size() - position_);
        if(amount <= 0)
            return -1;
        std::memcpy(dst, answer_.constData() + position_, size_t(amount));
        position_ += amount;
        return amount;
    }
private:
    QByteArray answer_;
    qint64 position_ = 0;
};
class FakeHttp : public QNetworkAccessManager
{
public:
    int calls = 0;
    int status = 200;
    QByteArray lastQuery;
    QUrl lastUrl;
    QByteArray lastAccept;
    QByteArray lastContentType;
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData) override
    {
        ++calls;
        lastUrl = request.url();
        lastAccept = request.rawHeader("Accept");
        lastContentType = request.header(QNetworkRequest::ContentTypeHeader).toByteArray();
        lastQuery = outgoingData ? outgoingData->readAll() : QByteArray();
        if(operation != PostOperation)
            return new FakeReply(request, {}, 405, this);
        return new FakeReply(request, srvAnswer(lastQuery), status, this);
    }
};

bool containsExpectedSrv(const QJDns::Response &response)
{
    for(const auto &record : response.answerRecords)
    {
        if(record.type == QJDns::Srv && record.port == 5222
           && record.name.contains("chat.example.com"))
            return true;
    }
    return false;
}
} // namespace

class DnsTransportTest : public QObject
{
    Q_OBJECT
private slots:
    void factoriesValidateEndpoints_data()
    {
        QTest::addColumn<QUrl>("endpoint");
        QTest::newRow("http") << QUrl("http://example.org/dns-query");
        QTest::newRow("no-host") << QUrl("https:///dns-query");
        QTest::newRow("credentials") << QUrl("https://user:pass@example.org/dns-query");
        QTest::newRow("fragment") << QUrl("https://example.org/dns-query#ignored");
        QTest::newRow("tcp-path") << QUrl("tcp://example.org/ignored");
        QTest::newRow("tcp-query") << QUrl("tcp://example.org?ignored");
        QTest::newRow("port-zero") << QUrl("tcp://example.org:0");
        QTest::newRow("unknown") << QUrl("udp://example.org");
    }

    void factoriesValidateEndpoints()
    {
        QFETCH(QUrl, endpoint);
        FakeHttp manager;
        QVERIFY(!QJDns::create(endpoint));
        QVERIFY(!QJDns::create(endpoint, QNetworkProxy(QNetworkProxy::NoProxy)));
        QVERIFY(!QJDns::create(endpoint, &manager, nullptr));
        QCOMPARE(manager.calls, 0);
    }

    void factoryOwnershipAndDefaults()
    {
        QObject parent;
        QPointer<QJDns> system = QJDns::create(&parent);
        QVERIFY(system);
        QCOMPARE(system->parent(), &parent);
        std::unique_ptr<QJDns> noParent(QJDns::create(nullptr));
        QVERIFY(noParent);
        std::unique_ptr<QJDns> http(QJDns::create(QUrl("https://example.org/dns-query")));
        QVERIFY(http);
        QPointer<QNetworkAccessManager> manager = http->findChild<QNetworkAccessManager *>();
        QVERIFY(manager);
        QCOMPARE(manager->proxy().type(), QNetworkProxy::DefaultProxy);
        http.reset();
        QVERIFY(!manager);
        std::unique_ptr<QJDns> tcp(QJDns::create(QUrl("tcp://127.0.0.1"),
                                               QNetworkProxy(QNetworkProxy::NoProxy)));
        QVERIFY(tcp);
        QPointer<QJDnsExternalTransport> transport = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> external(QJDns::create(transport));
        QVERIFY(external);
        external.reset();
        QVERIFY(!transport);
        QVERIFY(!QJDns::create(static_cast<QJDnsQueryTransport *>(nullptr), nullptr));
    }

    void factoryWithCallerManagerAndObjectRequest()
    {
        FakeHttp manager;
        std::unique_ptr<QJDns> dns(QJDns::create(QUrl("https://example.org/dns-query"), &manager));
        QVERIFY(dns);
        QCOMPARE(dns->parent(), nullptr);
        auto *request = dns->query("_xmpp-client._tcp.example.org", QJDns::Srv);
        QCOMPARE(request->parent(), dns.get());
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        QCOMPARE(manager.calls, 0);
        QCOMPARE(results.count(), 0);
        QTRY_COMPARE(results.count(), 1);
        QVERIFY(request->success());
        QCOMPARE(request->results().last().port, 5222);
        dns.reset();
        QCOMPARE(manager.parent(), nullptr);
        QCOMPARE(manager.calls, 1);
    }

    void objectRequestCacheHitIsDeferred()
    {
        auto *transport = new QJDnsExternalTransport;
        int calls = 0;
        connect(transport, &QJDnsExternalTransport::queryRequested, transport,
                [transport, &calls](int id, const QByteArray &packet) {
            ++calls;
            transport->completeRequest(id, srvAnswer(packet));
        });
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *first = dns->query("_xmpp-client._tcp.example.org", QJDns::Srv, this);
        QSignalSpy firstResults(first, &QJDnsSharedRequest::resultsReady);
        QTRY_COMPARE(firstResults.count(), 1);
        QVERIFY(first->success());
        auto *cached = dns->query("_xmpp-client._tcp.example.org", QJDns::Srv, this);
        QSignalSpy cachedResults(cached, &QJDnsSharedRequest::resultsReady);
        QCOMPARE(cachedResults.count(), 0);
        QTRY_COMPARE(cachedResults.count(), 1);
        QVERIFY(cached->success());
        QCOMPARE(calls, 1);
        dns.reset();
        QVERIFY(first->success()); // completed results outlive the resolver
        QCOMPARE(firstResults.count(), 1);
        QCOMPARE(cachedResults.count(), 1);
        QCOMPARE(cached->results().last().port, 5222);
        delete first;
        delete cached;
    }

    void objectRequestValidationIsDeferred_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<int>("type");
        QTest::newRow("empty") << QByteArray() << int(QJDns::A);
        QTest::newRow("nul") << QByteArray("bad\0name", 8) << int(QJDns::A);
        QTest::newRow("type-zero") << QByteArray("example.org") << 0;
        QTest::newRow("type-overflow") << QByteArray("example.org") << 65536;
        QTest::newRow("invalid-label") << (QByteArray(64, 'a') + ".org") << int(QJDns::A);
    }

    void objectRequestValidationIsDeferred()
    {
        QFETCH(QByteArray, name);
        QFETCH(int, type);
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *request = dns->query(name, type);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        QCOMPARE(results.count(), 0);
        QTRY_COMPARE(results.count(), 1);
        QVERIFY(!request->success());
        QCOMPARE(request->error(), QJDnsSharedRequest::ErrorGeneric);
        QCOMPARE(io.count(), 0);
    }

    void objectRequestUninitializedResolver()
    {
        QJDns dns;
        auto *request = dns.query("example.org", QJDns::A);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        QCOMPARE(results.count(), 0);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(request->error(), QJDnsSharedRequest::ErrorGeneric);
    }

    void objectRequestErrorMapping_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("expected");
        QTest::newRow("network") << 0 << int(QJDnsSharedRequest::ErrorGeneric);
        QTest::newRow("timeout") << 1 << int(QJDnsSharedRequest::ErrorTimeout);
        QTest::newRow("nxdomain") << 2 << int(QJDnsSharedRequest::ErrorNXDomain);
    }

    void objectRequestErrorMapping()
    {
        QFETCH(int, kind);
        QFETCH(int, expected);
        auto *transport = new QJDnsExternalTransport;
        connect(transport, &QJDnsExternalTransport::queryRequested, transport,
                [transport, kind](int id, QByteArray packet) {
            if(kind == 2)
            {
                packet[2] = char(0x81);
                packet[3] = char(0x83); // NXDOMAIN, no answer records
                transport->completeRequest(id, packet);
            }
            else
                transport->failRequest(id, "test", kind == 1 ? QJDnsQueryTransport::Timeout
                                                              : QJDnsQueryTransport::NetworkError);
        });
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *request = dns->query("example.org", QJDns::A);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        QCOMPARE(results.count(), 0);
        QTRY_COMPARE(results.count(), 1);
        QVERIFY(!request->success());
        QCOMPARE(int(request->error()), expected);
        QCoreApplication::processEvents();
        QCOMPARE(results.count(), 1);
    }

    void objectRequestCanDeleteItselfOnCompletion()
    {
        QJDns dns; // uninitialized: failure is still deferred
        QPointer<QJDnsSharedRequest> request = dns.query("example.org", QJDns::A);
        connect(request, &QJDnsSharedRequest::resultsReady, this, [request]() { delete request; });
        QVERIFY(request);
        QTRY_VERIFY(!request);
    }

    void objectRequestCancelOrDeleteBeforeStart()
    {
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        delete dns->query("deleted.example.org", QJDns::A);
        auto *request = dns->query("cancelled.example.org", QJDns::A);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        request->cancel();
        request->cancel();
        QCOMPARE(results.count(), 0);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(request->error(), QJDnsSharedRequest::ErrorCancelled);
        QCOMPARE(io.count(), 0);
    }

    void objectRequestsShareNetworkButNotLifetime()
    {
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        QSignalSpy cancelled(transport, &QJDnsExternalTransport::requestCancelled);
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *first = dns->query("_xmpp-client._tcp.example.org", QJDns::Srv);
        auto *second = dns->query("_xmpp-client._tcp.example.org", QJDns::Srv);
        QVERIFY(first != second);
        QSignalSpy results(second, &QJDnsSharedRequest::resultsReady);
        QTRY_COMPARE(io.count(), 1);
        delete first;
        QCOMPARE(cancelled.count(), 0);
        transport->completeRequest(io.first().at(0).toInt(), srvAnswer(io.first().at(1).toByteArray()));
        QTRY_COMPARE(results.count(), 1);
        QVERIFY(second->success());
        QCOMPARE(second->results().last().port, 5222);
    }

    void objectRequestLastConsumerCancelsNetwork()
    {
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        QSignalSpy cancelled(transport, &QJDnsExternalTransport::requestCancelled);
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *a = dns->query("same.example.org", QJDns::A);
        auto *b = dns->query("same.example.org", QJDns::A);
        QTRY_COMPARE(io.count(), 1);
        a->cancel();
        QCOMPARE(cancelled.count(), 0);
        delete b;
        QCOMPARE(cancelled.count(), 1);
    }

    void objectRequestRestartRetiresQueuedStart()
    {
        auto *transport = new QJDnsExternalTransport;
        connect(transport, &QJDnsExternalTransport::queryRequested, transport,
                [transport](int id, const QByteArray &packet) { transport->completeRequest(id, srvAnswer(packet)); });
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *request = dns->query("old.example.org", QJDns::Srv);
        request->cancel();
        request->query("new.example.org", QJDns::Srv);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        QTRY_COMPARE(results.count(), 1);
        QVERIFY(request->success());
        QCOMPARE(io.count(), 1);
        QVERIFY(io.first().at(1).toByteArray().contains("new"));
    }

    void backendCancellationMayDeleteConsumer()
    {
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        std::unique_ptr<QJDns> dns(QJDns::create(transport));
        QVERIFY(dns);
        QPointer<QJDnsSharedRequest> request = dns->query("example.org", QJDns::A);
        connect(transport, &QJDnsExternalTransport::requestCancelled, this,
                [&request](int) { delete request; });
        QTRY_COMPARE(io.count(), 1);
        request->cancel();
        QVERIFY(!request);
    }

    void objectRequestResolverDestroyed_data()
    {
        QTest::addColumn<bool>("started");
        QTest::newRow("before-start") << false;
        QTest::newRow("in-flight") << true;
    }

    void objectRequestResolverDestroyed()
    {
        QFETCH(bool, started);
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        auto *dns = QJDns::create(transport, nullptr);
        QVERIFY(dns);
        auto *request = dns->query("example.org", QJDns::A, this);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        if(started)
            QTRY_COMPARE(io.count(), 1);
        delete dns;
        QCOMPARE(results.count(), 0);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(request->error(), QJDnsSharedRequest::ErrorResolverDestroyed);
        delete request;
    }

    void objectRequestResolverShutdownCompletesRequest()
    {
        auto *transport = new QJDnsExternalTransport;
        QSignalSpy io(transport, &QJDnsExternalTransport::queryRequested);
        std::unique_ptr<QJDns> dns(QJDns::create(transport, nullptr));
        QVERIFY(dns);
        auto *request = dns->query("example.org", QJDns::A);
        QSignalSpy results(request, &QJDnsSharedRequest::resultsReady);
        QTRY_COMPARE(io.count(), 1);
        dns->shutdown();
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(request->error(), QJDnsSharedRequest::ErrorNoNet);
    }

    void legacySharedRequestStillReportsAndCancels()
    {
        QJDnsShared resolver(QJDnsShared::UnicastInternet);
        QJDnsSharedRequest request(&resolver);
        QSignalSpy results(&request, &QJDnsSharedRequest::resultsReady);
        request.query("example.org", QJDns::A);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(request.error(), QJDnsSharedRequest::ErrorNoNet);
        request.query("example.org", QJDns::A);
        request.cancel(); // legacy cancellation stays silent
        QCoreApplication::processEvents();
        QCOMPARE(results.count(), 1);
    }

    void udpUnicastStillUsesConfiguredNameserver_data()
    {
        QTest::addColumn<bool>("factory");
        QTest::newRow("legacy-init") << false;
        QTest::newRow("system-factory") << true;
    }

    void udpUnicastStillUsesConfiguredNameserver()
    {
        QFETCH(bool, factory);
        QUdpSocket server;
        QVERIFY(server.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
        int queryCount = 0;
        connect(&server, &QUdpSocket::readyRead, &server, [&]() {
            while(server.hasPendingDatagrams()) {
                QByteArray query;
                query.resize(int(server.pendingDatagramSize()));
                QHostAddress sender;
                quint16 port = 0;
                if(server.readDatagram(query.data(), query.size(), &sender, &port) < 0)
                    return;
                ++queryCount;
                const QByteArray reply = srvAnswer(query);
                server.writeDatagram(reply, sender, port);
            }
        });
        std::unique_ptr<QJDns> owner(factory ? QJDns::create() : new QJDns);
        QVERIFY(owner);
        QJDns &dns = *owner;
        if(!factory)
            QVERIFY(dns.init(QJDns::Unicast, QHostAddress::AnyIPv4));
        QJDns::NameServer ns;
        ns.address = QHostAddress::LocalHost;
        ns.port = server.localPort();
        QVERIFY(dns.setNameServers({ns}));
        bool success = false;
        connect(&dns, &QJDns::resultsReady, &dns,
                [&](int, const QJDns::Response &r) { success = containsExpectedSrv(r); });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(success, 5000);
        QCOMPARE(queryCount, 1);
    }

    void nativeQueryTransportRejectsNameserverListAndMulticast()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        QVERIFY(!dns.init(QJDns::Multicast, QHostAddress::AnyIPv4));
        QJDns::NameServer ns;
        ns.address = QHostAddress::LocalHost;
        QVERIFY(!dns.setNameServers({ns}));
    }

    void cancellingLastCoalescedConsumerAbortsBackend()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        int dispatched = 0;
        int cancelled = 0;
        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [&](int, const QByteArray &) { ++dispatched; });
        connect(external, &QJDnsExternalTransport::requestCancelled, &dns,
                [&](int) { ++cancelled; });
        const int a = dns.queryStart("coalesce.example.com.", QJDns::Srv);
        const int b = dns.queryStart("coalesce.example.com.", QJDns::Srv);
        QVERIFY(a >= 0 && b >= 0 && a != b);
        QTRY_COMPARE_WITH_TIMEOUT(dispatched, 1, 5000);
        dns.queryCancel(a);
        QCOMPARE(cancelled, 0);
        dns.queryCancel(b);
        QCOMPARE(cancelled, 1);
        QCOMPARE(dispatched, 1);
    }

    void timeoutIsNotAProtocolError()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        external->setTimeout(40);
        int seen = 0;
        QJDns::Error kind = QJDns::ErrorGeneric;
        connect(&dns, &QJDns::error, &dns, [&](int, QJDns::Error err) {
            ++seen;
            kind = err;
        });
        QVERIFY(dns.queryStart("timeout.example.com.", QJDns::Srv) >= 0);
        QTRY_COMPARE_WITH_TIMEOUT(seen, 1, 5000);
        QCOMPARE(kind, QJDns::ErrorTimeout);
    }

    void malformedResponseIsRejectedByCore()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [external](int id, const QByteArray &query) {
            QByteArray forged = srvAnswer(query);
            forged[0] = char(quint8(forged.at(0)) ^ 0x80); // wrong DNS transaction ID
            external->completeRequest(id, forged);
        });
        bool failed = false;
        connect(&dns, &QJDns::error, &dns, [&](int, QJDns::Error kind) {
            failed = kind == QJDns::ErrorGeneric;
        });
        QVERIFY(dns.queryStart("forged.example.com.", QJDns::Srv) >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(failed, 5000);
    }

    void tcpFramingAndSrvParsing()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&]() {
            auto *client = server.nextPendingConnection();
            auto received = std::make_shared<QByteArray>();
            connect(client, &QTcpSocket::readyRead, client, [client, received, &requests]() {
                *received += client->readAll();
                if(received->size() < 2)
                    return;
                const int frameSize = get16(*received, 0);
                if(received->size() < frameSize + 2)
                    return;
                const QByteArray answer = srvAnswer(received->mid(2, frameSize));
                QByteArray framed;
                put16(&framed, quint16(answer.size()));
                framed += answer;
                ++requests;
                client->write(framed.left(3)); // exercise fragmented TCP framing
                QTimer::singleShot(10, client, [client, framed]() { client->write(framed.mid(3)); });
            });
        });

        std::unique_ptr<QJDns> resolverOwner(QJDns::create(
            QUrl(QStringLiteral("tcp://127.0.0.1:%1").arg(server.serverPort())),
            QNetworkProxy(QNetworkProxy::NoProxy)));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        bool success = false;
        bool failure = false;
        connect(&dns, &QJDns::resultsReady, &dns, [&](int, const QJDns::Response &response) {
            success = containsExpectedSrv(response);
        });
        connect(&dns, &QJDns::error, &dns, [&](int, QJDns::Error) { failure = true; });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(success || failure, 5000);
        QVERIFY(success);
        QVERIFY(!failure);
        QCOMPARE(requests, 1);
    }

    void tcpOverSocks5ResolvesDnsServerRemotely()
    {
        QTcpServer socks;
        QVERIFY(socks.listen(QHostAddress::LocalHost, 0));
        QString connectHost;
        int responses = 0;
        connect(&socks, &QTcpServer::newConnection, &socks, [&]() {
            auto *client = socks.nextPendingConnection();
            struct Session {
                enum State { Greeting, Connect, Data } state = Greeting;
                QByteArray buffer;
            };
            auto session = std::make_shared<Session>();
            connect(client, &QTcpSocket::readyRead, client, [&, client, session]() {
                session->buffer += client->readAll();
                for(;;)
                {
                    if(session->state == Session::Greeting)
                    {
                        if(session->buffer.size() < 2)
                            return;
                        const int methods = quint8(session->buffer.at(1));
                        if(session->buffer.size() < methods + 2)
                            return;
                        session->buffer.remove(0, methods + 2);
                        client->write(QByteArray::fromHex("0500")); // SOCKS5, no authentication
                        session->state = Session::Connect;
                    }
                    else if(session->state == Session::Connect)
                    {
                        if(session->buffer.size() < 5)
                            return;
                        if(quint8(session->buffer.at(0)) != 5 ||
                           quint8(session->buffer.at(1)) != 1 ||
                           quint8(session->buffer.at(3)) != 3)
                            return;
                        const int length = quint8(session->buffer.at(4));
                        if(session->buffer.size() < length + 7)
                            return;
                        connectHost = QString::fromLatin1(session->buffer.mid(5, length));
                        session->buffer.remove(0, length + 7);
                        client->write(QByteArray::fromHex("050000017f0000010035"));
                        session->state = Session::Data;
                    }
                    else
                    {
                        if(session->buffer.size() < 2)
                            return;
                        const int length = get16(session->buffer, 0);
                        if(session->buffer.size() < length + 2)
                            return;
                        const QByteArray answer = srvAnswer(session->buffer.mid(2, length));
                        QByteArray framed;
                        put16(&framed, quint16(answer.size()));
                        framed += answer;
                        session->buffer.remove(0, length + 2);
                        ++responses;
                        client->write(framed);
                        return;
                    }
                }
            });
        });

        std::unique_ptr<QJDns> resolverOwner(QJDns::create(
            QUrl(QStringLiteral("tcp://dns-server.invalid")),
            QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), socks.serverPort())));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        bool success = false;
        connect(&dns, &QJDns::resultsReady, &dns, [&](int, const QJDns::Response &answer) {
            success = containsExpectedSrv(answer);
        });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(success, 5000);
        QCOMPARE(connectHost, QStringLiteral("dns-server.invalid"));
        QCOMPARE(responses, 1);
    }

    void httpCreatesAndOwnsDefaultManager()
    {
        // Omitting QNAM must create one, parented to the HTTP transport.
        // This test performs no network requests.
        QPointer<QNetworkAccessManager> owned;
        {
            auto *transport = new QJDnsHttpTransport(QUrl(QStringLiteral("https://example.test/dns-query")));
            std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
            QVERIFY(resolverOwner);

            auto *manager = transport->findChild<QNetworkAccessManager *>();
            QVERIFY(manager);
            QCOMPARE(transport->networkAccessManager(), manager);
            QCOMPARE(manager->parent(), static_cast<QObject *>(transport));
            // Caller can configure a proxy without taking ownership of QNAM.
            const QNetworkProxy socks(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), 1337);
            transport->networkAccessManager()->setProxy(socks);
            QCOMPARE(manager->proxy().type(), QNetworkProxy::Socks5Proxy);
            owned = manager;
            QVERIFY(owned);
        }
        QVERIFY(owned.isNull());
    }

    void httpDoesNotTakeOwnershipOfExternalManager()
    {
        // External managers must never be reparented or deleted by QJDns.
        QObject owner;
        FakeHttp manager;
        manager.setParent(&owner);
        QPointer<QNetworkAccessManager> observed(&manager);
        {
            auto *transport = new QJDnsHttpTransport(
                QUrl(QStringLiteral("https://example.test/dns-query")), &manager);
            std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
            QVERIFY(resolverOwner);

            QVERIFY(transport->findChildren<QNetworkAccessManager *>().isEmpty());
            QCOMPARE(transport->networkAccessManager(), static_cast<QNetworkAccessManager *>(&manager));
            QCOMPARE(manager.parent(), &owner);
        }
        QVERIFY(observed);
        QCOMPARE(manager.parent(), &owner);
    }

    void httpUsesCallerManagerAndJdnsParser()
    {
        FakeHttp manager;
        const QUrl url(QStringLiteral("https://example.test/dns-query"));
        auto *transport = new QJDnsHttpTransport(url, &manager);
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        bool success = false;
        connect(&dns, &QJDns::resultsReady, &dns, [&](int, const QJDns::Response &response) {
            success = containsExpectedSrv(response);
        });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(success, 5000);
        QCOMPARE(manager.calls, 1);
        QCOMPARE(manager.lastUrl, url);
        QCOMPARE(manager.lastAccept, QByteArray("application/dns-message"));
        QCOMPARE(manager.lastContentType, QByteArray("application/dns-message"));
        QVERIFY(manager.lastQuery.size() >= 12);
    }

    void httpTransportRejectsPlainHttpWithoutNetworkAccess()
    {
        FakeHttp manager;
        auto *transport = new QJDnsHttpTransport(
            QUrl(QStringLiteral("http://example.test/dns-query")), &manager);
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;


        bool rejected = false;
        connect(&dns, &QJDns::error, &dns,
                [&](int, QJDns::Error kind) {
            rejected = kind == QJDns::ErrorGeneric;
        });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(rejected, 5000);
        QCOMPARE(manager.calls, 0); // Must not perform a plaintext HTTP request.
    }

    void httpRequestFailureNeverFallsBack()
    {
        FakeHttp manager;
        manager.status = 503;
        auto *transport = new QJDnsHttpTransport(QUrl(QStringLiteral("https://example.test/dns-query")),
                                                &manager);
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        bool failed = false;
        connect(&dns, &QJDns::error, &dns, [&](int, QJDns::Error) { failed = true; });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(failed, 5000);
        QCOMPARE(manager.calls, 1);
    }

    void tcpFailureNeverFallsBack()
    {
        QTcpServer temp;
        QVERIFY(temp.listen(QHostAddress::LocalHost, 0));
        const quint16 closedPort = temp.serverPort();
        temp.close();

        auto *transport = new QJDnsTcpTransport(QStringLiteral("127.0.0.1"), closedPort,
                                                QNetworkProxy(QNetworkProxy::NoProxy));
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        bool failed = false;
        connect(&dns, &QJDns::error, &dns, [&](int, QJDns::Error) { failed = true; });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(failed, 5000);
    }

    void tcpTimeoutNeverFallsBack()
    {
        // The server accepts the connection but never answers: the request
        // times out (retiring its timer from the timer's own signal) and JDNS
        // gets an error instead of trying another resolver.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        QList<QTcpSocket *> accepted;
        connect(&silent, &QTcpServer::newConnection, &silent, [&]() {
            while(QTcpSocket *socket = silent.nextPendingConnection())
                accepted += socket;
        });

        auto *transport = new QJDnsTcpTransport(QStringLiteral("127.0.0.1"), silent.serverPort(),
                                                QNetworkProxy(QNetworkProxy::NoProxy));
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        transport->setTimeout(50);
        bool failed = false;
        connect(&dns, &QJDns::error, &dns, [&](int, QJDns::Error) { failed = true; });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(failed, 5000);
        QCOMPARE(accepted.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(accepted.first()->state(), QAbstractSocket::UnconnectedState, 5000);
    }

    void repeatedInitDoesNotReplaceLiveSession()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [external](int id, const QByteArray &query) {
            external->completeRequest(id, srvAnswer(query));
        });
        QVERIFY(!dns.init(QJDns::Unicast, QHostAddress::AnyIPv4));
        QVERIFY(!dns.init(QJDns::Multicast, QHostAddress::AnyIPv4));
        bool success = false;
        connect(&dns, &QJDns::resultsReady, &dns,
                [&](int, const QJDns::Response &answer) {
            success = containsExpectedSrv(answer);
        });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) >= 0);
        QTRY_VERIFY_WITH_TIMEOUT(success, 5000);
    }

    void externalBackendUsesExistingSrvParser()
    {
        // Models an application-owned async DoH client. No HTTP
        // implementation or QNetworkAccessManager is passed to JDNS.
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        int calls = 0;
        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [external, &calls](int requestId, const QByteArray &request) {
            ++calls;
            external->completeRequest(requestId, srvAnswer(request));
        });

        bool success = false;
        connect(&dns, &QJDns::resultsReady, &dns,
                [&success](int, const QJDns::Response &answer) {
            success = containsExpectedSrv(answer);
        });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(success, 5000);
        QCOMPARE(calls, 1);
    }

    void externalConcurrentQueriesCompleteOutOfOrder()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        QHash<int, QByteArray> requests;
        QList<int> issuedOrder;
        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [external, &requests, &issuedOrder](int id, const QByteArray &packet) {
            requests.insert(id, packet);
            issuedOrder.append(id);
            if(issuedOrder.size() == 2) {
                external->completeRequest(issuedOrder.at(1), srvAnswer(requests.value(issuedOrder.at(1))));
                external->completeRequest(issuedOrder.at(0), srvAnswer(requests.value(issuedOrder.at(0))));
            }
        });
        int completed = 0;
        connect(&dns, &QJDns::resultsReady, &dns,
                [&](int, const QJDns::Response &response) {
            if(containsExpectedSrv(response))
                ++completed;
        });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QVERIFY(dns.queryStart("_xmpp-client._tcp.another.example.", QJDns::Srv) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 5000);
        QCOMPARE(requests.size(), 2);
    }

    void externalQueryWithUnencodableNameFails()
    {
        // A label longer than 63 bytes cannot be put on the wire. JDNS has no
        // retry timer for a managed transport, so it must fail the query.
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        int calls = 0;
        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [&calls](int, const QByteArray &) { ++calls; });

        int failed = -1;
        connect(&dns, &QJDns::error, &dns,
                [&failed](int id, QJDns::Error) { failed = id; });
        const int id = dns.queryStart(QByteArray(64, 'a') + ".example.", QJDns::A);
        QVERIFY(id > 0);
        QTRY_COMPARE_WITH_TIMEOUT(failed, id, 5000);
        QCOMPARE(calls, 0);
    }

    void externalBackendFailureCannotUseLocalDns()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        int calls = 0;
        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [external, &calls](int id, const QByteArray &) {
            ++calls;
            external->failRequest(id, QStringLiteral("TLS handshake rejected"));
        });

        bool failed = false;
        connect(&dns, &QJDns::error, &dns,
                [&failed](int, QJDns::Error) { failed = true; });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(failed, 5000);
        QCOMPARE(calls, 1);
    }

    void externalTimeoutCancelsBackendAndRejectsLateResponse()
    {
        auto *external = new QJDnsExternalTransport;
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(external, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        external->setTimeout(30);
        int requestId = -1;
        QByteArray original;
        bool cancelled = false;
        connect(external, &QJDnsExternalTransport::queryRequested, &dns,
                [&](int id, const QByteArray &packet) {
            requestId = id;
            original = packet;
        });
        connect(external, &QJDnsExternalTransport::requestCancelled, &dns,
                [&](int id) {
            cancelled = id == requestId;
        });
        bool failed = false;
        connect(&dns, &QJDns::error, &dns,
                [&](int, QJDns::Error) { failed = true; });
        QVERIFY(dns.queryStart("_xmpp-client._tcp.example.com.", QJDns::Srv) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(failed, 5000);
        QVERIFY(cancelled);
        QVERIFY(requestId >= 0);
        external->completeRequest(requestId, srvAnswer(original));
        // Completion after timeout must not schedule another DNS result.
        QCoreApplication::processEvents();
        QVERIFY(cancelled);
    }

    void invalidExternalQueryCancelledBeforeDeferredError()
    {
        QJDnsExternalTransport backend;
        int errors = 0;
        int requests = 0;
        connect(&backend, &QJDnsQueryTransport::failed, &backend,
                [&](int, const QString &, QJDnsQueryTransport::Failure) { ++errors; });
        connect(&backend, &QJDnsExternalTransport::queryRequested, &backend,
                [&](int, const QByteArray &) { ++requests; });
        backend.startRequest(51, QByteArray("bad"));
        backend.cancelRequest(51);
        QCoreApplication::processEvents();
        QCOMPARE(errors, 0);
        QCOMPARE(requests, 0);

        backend.startRequest(52, QByteArray("bad"));
        QTRY_COMPARE_WITH_TIMEOUT(errors, 1, 1000);
        QCOMPARE(requests, 0);
    }

    void externalCloseNotifiesBackendAndIgnoresLateResult()
    {
        QJDnsExternalTransport external;
        QByteArray request(12, '\0');
        request += encodeName("_xmpp-client._tcp.example.org");
        put16(&request, 33);
        put16(&request, 1);
        int cancelledId = 0;
        int dispatched = 0;
        int responses = 0;
        connect(&external, &QJDnsExternalTransport::requestCancelled, &external,
                [&](int id) { cancelledId = id; });
        connect(&external, &QJDnsExternalTransport::queryRequested, &external,
                [&](int, const QByteArray &) { ++dispatched; });
        connect(&external, &QJDnsQueryTransport::responseReady, &external,
                [&](int, const QByteArray &) { ++responses; });
        external.startRequest(42, request);
        QTRY_COMPARE_WITH_TIMEOUT(dispatched, 1, 3000);
        external.cancelRequest(42);
        QCOMPARE(cancelledId, 42);
        external.completeRequest(42, srvAnswer(request));
        QCoreApplication::processEvents();
        QCOMPARE(responses, 0);
    }

    void queryTransportCannotPublishMulticast()
    {
        auto *transport = new QJDnsTcpTransport(QStringLiteral("127.0.0.1"), 53,
                                                QNetworkProxy(QNetworkProxy::NoProxy));
        std::unique_ptr<QJDns> resolverOwner(QJDns::create(transport, nullptr));
        QVERIFY(resolverOwner);
        QJDns &dns = *resolverOwner;

        QVERIFY(!dns.init(QJDns::Multicast, QHostAddress::AnyIPv4));
    }
};
QTEST_MAIN(DnsTransportTest)
#include "transports.moc"
