// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QEventLoop>
#include <QTest>
#include <QTimer>
#include <QtCrypto>
#include <ncrypt.h>
#include <windows.h>
#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif

namespace {
const QString Provider = QStringLiteral("qca-wincrypto");
struct Identity
{
    QCA::PrivateKey  key;
    QCA::Certificate certificate;
    Identity()
    {
        key = QCA::KeyGenerator().createRSA(2048, 65537, Provider);
        if (key.isNull())
            return;
        QCA::CertificateOptions options;
        QCA::CertificateInfo    info;
        info.insert(QCA::CommonName, QStringLiteral("qca.test"));
        info.insert(QCA::DNS, QStringLiteral("qca.test"));
        options.setInfo(info);
        options.setSerialNumber(QCA::BigInteger(42));
        const auto now = QDateTime::currentDateTimeUtc();
        options.setValidityPeriod(now.addSecs(-60), now.addDays(1));
        options.setConstraints({QCA::DigitalSignature, QCA::KeyEncipherment, QCA::ClientAuth, QCA::ServerAuth});
        certificate = QCA::Certificate(options, key, Provider);
    }
};
QStringList temporaryKeys()
{
    QStringList        result;
    NCRYPT_PROV_HANDLE provider = 0;
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
        return {QStringLiteral("enumeration failed")};
    void           *state = nullptr;
    NCryptKeyName  *name  = nullptr;
    SECURITY_STATUS status;
    while ((status = NCryptEnumKeys(provider, nullptr, &name, &state, 0)) == ERROR_SUCCESS) {
        const auto value = QString::fromWCharArray(name->pszName);
        if (value.startsWith(QStringLiteral("qca-wincrypto-%1-").arg(QCoreApplication::applicationPid())))
            result += value;
        NCryptFreeBuffer(name);
    }
    if (status != NTE_NO_MORE_ITEMS)
        result += QStringLiteral("enumeration failed");
    if (state)
        NCryptFreeBuffer(state);
    NCryptFreeObject(provider);
    result.sort();
    return result;
}
class Pair : public QObject
{
public:
    Identity    identity;
    QCA::TLS    client {QCA::TLS::Datagram, this, Provider};
    QCA::TLS    server {QCA::TLS::Datagram, this, Provider};
    bool        clientDone = false, serverDone = false, failed = false, oversized = false, dropped = false;
    bool        pauseClient = false, reverse = false;
    int         drop = 0, mtu = 1400;
    QEventLoop *loop = nullptr;
    explicit Pair(int packetMTU = 1400, bool trusted = true)
        : mtu(packetMTU)
    {
        QCA::CertificateCollection trust;
        trust.addCertificate(identity.certificate);
        if (trusted)
            client.setTrustedCertificates(trust);
        server.setTrustedCertificates(trust);
        QCA::CertificateChain chain;
        chain += identity.certificate;
        client.setCertificate(chain, identity.key);
        server.setCertificate(chain, identity.key);
        client.setPacketMTU(mtu);
        server.setPacketMTU(mtu);
        connect(&client, &QCA::TLS::readyReadOutgoing, this, [this] {
            if (!pauseClient)
                transfer(client, server, false);
        });
        connect(&server, &QCA::TLS::readyReadOutgoing, this, [this] { transfer(server, client, true); });
        const auto done = [this](QCA::TLS &tls, bool &flag) {
            flag = true;
            tls.continueAfterStep();
            if (clientDone && serverDone && loop)
                loop->quit();
        };
        connect(&client, &QCA::TLS::handshaken, this, [this, done] { done(client, clientDone); });
        connect(&server, &QCA::TLS::handshaken, this, [this, done] { done(server, serverDone); });
        for (auto tls : {&client, &server}) {
            connect(tls, &QCA::TLS::peerCertificateAvailable, tls, &QCA::TLS::continueAfterStep);
            connect(tls, &QCA::TLS::error, this, [this] {
                failed = true;
                if (loop)
                    loop->quit();
            });
        }
    }
    bool run()
    {
        QEventLoop events;
        QTimer     deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &events, &QEventLoop::quit);
        loop = &events;
        server.startServer();
        client.startClient(QStringLiteral("qca.test"));
        deadline.start(10000);
        if (!failed && !(clientDone && serverDone))
            events.exec();
        loop = nullptr;
        return clientDone && serverDone && !failed && !oversized;
    }
    void transfer(QCA::TLS &from, QCA::TLS &to, bool fromServer)
    {
        QList<QByteArray> packets;
        while (from.packetsOutgoingAvailable())
            packets.append(from.readOutgoing());
        if (reverse)
            std::reverse(packets.begin(), packets.end());
        for (const auto &packet : packets) {
            oversized |= packet.size() > mtu;
            const bool final =
                packet.size() > 5 && uchar(packet[0]) == 22 && uchar(packet[3]) == 0 && uchar(packet[4]) == 1;
            if (!dropped &&
                ((drop == 1 && !fromServer) || (drop == 2 && fromServer) || (drop == 3 && fromServer && final))) {
                dropped = true;
                continue;
            }
            to.writeIncoming(packet);
        }
    }
};
}
class WinCryptoDTLSTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void handshake_data();
    void handshake();
    void datagrams();
    void constraintsAndTrust();
    void identityCleanup();

private:
    QCA::Initializer _qca;
};

void WinCryptoDTLSTest::handshake_data()
{
    QTest::addColumn<QString>("profile");
    QTest::addColumn<int>("drop");
    QTest::addColumn<bool>("reverse");
    QTest::addColumn<int>("mtu");
    QTest::newRow("data-only") << QString() << 0 << false << 1400;
    const QString cm = QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80");
    for (const auto &profile : {cm,
                                QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_32"),
                                QStringLiteral("SRTP_AEAD_AES_128_GCM"),
                                QStringLiteral("SRTP_AEAD_AES_256_GCM")})
        QTest::newRow(qPrintable(profile)) << profile << 0 << false << 1400;
    QTest::newRow("lost-client-hello") << cm << 1 << false << 1400;
    QTest::newRow("lost-cookie") << cm << 2 << false << 1400;
    QTest::newRow("lost-server-finished") << cm << 3 << false << 1400;
    QTest::newRow("reordered-fragments") << cm << 0 << true << 300;
    QTest::newRow("small-MTU") << cm << 0 << false << 300;
}
void WinCryptoDTLSTest::handshake()
{
    QFETCH(QString, profile);
    QFETCH(int, drop);
    QFETCH(bool, reverse);
    QFETCH(int, mtu);
    Pair pair(mtu);
    QVERIFY(!pair.identity.key.isNull());
    QVERIFY(!pair.identity.certificate.isNull());
    pair.drop                  = drop;
    pair.reverse               = reverse;
    const QStringList profiles = profile.isEmpty() ? QStringList() : QStringList {profile};
    QVERIFY(pair.client.setSRTPProfiles(profiles));
    QVERIFY(pair.server.setSRTPProfiles(profiles));
    QVERIFY(pair.run());
    QCOMPARE(pair.dropped, drop != 0);
    QCOMPARE(pair.client.version(), QCA::TLS::DTLS_v1_2);
    QCOMPARE(pair.server.peerIdentityResult(), QCA::TLS::Valid);
    QCOMPARE(pair.client.peerIdentityResult(), QCA::TLS::Valid);
    QCOMPARE(pair.client.selectedSRTPProfile(), profile);
    QCOMPARE(pair.server.selectedSRTPProfile(), profile);
    const auto client = pair.client.srtpKeyingMaterial(), server = pair.server.srtpKeyingMaterial();
    if (profile.isEmpty()) {
        QVERIFY(client.isNull());
        QVERIFY(server.isNull());
        return;
    }
    const int keySize  = profile == QLatin1String("SRTP_AEAD_AES_256_GCM") ? 32 : 16;
    const int saltSize = profile.contains(QLatin1String("GCM")) ? 12 : 14;
    QCOMPARE(client.localMasterKey().size(), keySize);
    QCOMPARE(client.localMasterSalt().size(), saltSize);
    QVERIFY(client.localMasterKey() == server.remoteMasterKey());
    QVERIFY(client.localMasterSalt() == server.remoteMasterSalt());
    QVERIFY(client.remoteMasterKey() == server.localMasterKey());
    QVERIFY(client.remoteMasterSalt() == server.localMasterSalt());
    QVERIFY(client.localMasterKey() != client.remoteMasterKey());
    pair.client.reset();
    QVERIFY(pair.client.srtpKeyingMaterial().isNull());
}
void WinCryptoDTLSTest::datagrams()
{
    Pair pair;
    QVERIFY(pair.run());
    pair.pauseClient = true;
    QList<QByteArray> received;
    connect(&pair.server, &QCA::TLS::readyRead, this, [&] {
        while (pair.server.packetsAvailable())
            received.append(pair.server.read());
    });
    QList<QByteArray> messages;
    for (int i = 0; i < 32; ++i) {
        messages.append(QByteArray(512 + i, char(i)));
        pair.client.write(messages.back());
    }
    QTRY_COMPARE(pair.client.packetsOutgoingAvailable(), 32);
    QList<QByteArray> records;
    while (pair.client.packetsOutgoingAvailable())
        records.append(pair.client.readOutgoing());
    auto corrupted                  = records.front();
    corrupted[corrupted.size() - 1] = char(corrupted.at(corrupted.size() - 1) ^ 1);
    pair.server.writeIncoming(corrupted);
    // A forged, unencrypted close_notify must not close an established session.
    pair.server.writeIncoming(QByteArray::fromHex("15fefd000000000000000000020100"));
    for (auto record = records.crbegin(); record != records.crend(); ++record)
        pair.server.writeIncoming(*record);
    pair.server.writeIncoming(records.back()); // Replay must not deliver plaintext again.
    QTRY_COMPARE(received.size(), messages.size());
    std::reverse(messages.begin(), messages.end());
    QCOMPARE(received, messages);
    QVERIFY(!pair.failed);
    received.clear();
    pair.pauseClient = false;
    const QByteArray bulk(1024, 'b');
    for (int i = 0; i < 4096; ++i)
        pair.client.write(bulk);
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), 4096, 10000);
    for (const auto &packet : received)
        QCOMPARE(packet, bulk);
    QVERIFY(!pair.failed);
    pair.pauseClient = true;
    pair.server.write(QByteArray("reply"));
    QTRY_COMPARE(pair.client.packetsAvailable(), 1);
    QCOMPARE(pair.client.read(), QByteArray("reply"));
    pair.client.close();
    // pauseClient lets the test inspect and deliver the encrypted alert.
    QTRY_VERIFY(pair.client.packetsOutgoingAvailable() > 0);
    const auto close = pair.client.readOutgoing();
    QVERIFY(!close.isEmpty());
    pair.server.writeIncoming(close);
    QTest::qWait(20);
    QVERIFY(!pair.failed);
}
void WinCryptoDTLSTest::constraintsAndTrust()
{
    Pair untrusted(1400, false);
    QVERIFY(untrusted.run());
    QVERIFY(untrusted.client.peerIdentityResult() != QCA::TLS::Valid);
    Pair mismatch;
    QVERIFY(mismatch.client.setSRTPProfiles({QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80")}));
    QVERIFY(mismatch.server.setSRTPProfiles({QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_32")}));
    QVERIFY(!mismatch.run());
    QVERIFY(mismatch.client.srtpKeyingMaterial().isNull());
    Pair impossible;
    impossible.client.setConstraints(512, 1024);
    QVERIFY(!impossible.run());
    Pair unavailable;
    unavailable.client.setConstraints(QStringList {QStringLiteral("TLS_UNAVAILABLE_CIPHER")});
    QVERIFY(!unavailable.run());
    QVERIFY(unavailable.client.srtpKeyingMaterial().isNull());
    Pair oversized(300);
    QVERIFY(oversized.run());
    oversized.client.write(QByteArray(1024, 'x'));
    QTRY_VERIFY(oversized.failed);
    QCOMPARE(oversized.server.packetsAvailable(), 0);
}
void WinCryptoDTLSTest::identityCleanup()
{
    const auto before = temporaryKeys();
    QVERIFY(!before.contains(QStringLiteral("enumeration failed")));
    for (int i = 0; i < 3; ++i) {
        Pair pair;
        if (i == 1) {
            QCA::ConvertResult result;
            const auto         loaded = QCA::PrivateKey::fromDER(pair.identity.key.toDER(), {}, &result, Provider);
            QCOMPARE(result, QCA::ConvertGood);
            QVERIFY(!loaded.isNull());
            QCA::CertificateChain chain;
            chain += pair.identity.certificate;
            pair.client.setCertificate(chain, loaded);
            pair.server.setCertificate(chain, loaded);
        }
        QVERIFY(pair.run());
        QCOMPARE(temporaryKeys(), before); // Removed before session teardown.
    }
    Pair       mismatch;
    const auto otherKey = QCA::KeyGenerator().createRSA(2048, 65537, Provider);
    QVERIFY(!otherKey.isNull());
    QCA::CertificateChain chain;
    chain += mismatch.identity.certificate;
    mismatch.server.setCertificate(chain, otherKey);
    QVERIFY(!mismatch.run());
    QCOMPARE(temporaryKeys(), before);
}
QTEST_GUILESS_MAIN(WinCryptoDTLSTest)
#include "wincryptodtlsunittest.moc"
