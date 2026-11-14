/*
 * Copyright (C) 2026 QCA contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <QtCrypto>

#include <QSignalSpy>
#include <QtTest/QtTest>

class SaslChannelBindingUnitTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void scramSha256PlusClient();
    void scramSha256PlusClientAfterParams();
    void completeClient_data();
    void completeClient();

private:
    QCA::Initializer *m_init = nullptr;
};

void SaslChannelBindingUnitTest::initTestCase()
{
    m_init = new QCA::Initializer;
}

void SaslChannelBindingUnitTest::cleanupTestCase()
{
    delete m_init;
    m_init = nullptr;
}

void SaslChannelBindingUnitTest::scramSha256PlusClient()
{
    const QString provider = QStringLiteral("qca-cyrus-sasl");
    if (!QCA::isSupported("sasl", provider))
        QSKIP("qca-cyrus-sasl is not available");

    QCA::SASL sasl(nullptr, provider);
    if (!sasl.supportsChannelBinding())
        QSKIP("The installed Cyrus SASL does not provide SASL_CHANNEL_BINDING");

    const QByteArray channelBindingData(32, '\x5a');
    QVERIFY(sasl.setChannelBinding(QStringLiteral("tls-exporter"), channelBindingData, true));

    sasl.setUsername(QStringLiteral("user"));
    sasl.setPassword(QCA::SecureArray("pencil"));

    QSignalSpy startedSpy(&sasl, &QCA::SASL::clientStarted);
    QSignalSpy errorSpy(&sasl, &QCA::SASL::error);

    sasl.startClient(
        QStringLiteral("xmpp"), QStringLiteral("example.test"), QStringList {QStringLiteral("SCRAM-SHA-256-PLUS")});

    QTRY_VERIFY_WITH_TIMEOUT(!startedSpy.isEmpty() || !errorSpy.isEmpty(), 5000);
    if (!errorSpy.isEmpty() && sasl.authCondition() == QCA::SASL::NoMechanism)
        QSKIP("The Cyrus SASL SCRAM-SHA-256 plugin is not installed");

    QVERIFY2(errorSpy.isEmpty(), "Unable to start SCRAM-SHA-256-PLUS");
    QCOMPARE(startedSpy.size(), 1);
    QCOMPARE(sasl.mechanism(), QStringLiteral("SCRAM-SHA-256-PLUS"));

    const QList<QVariant> arguments = startedSpy.constFirst();
    QVERIFY(arguments.at(0).toBool());
    QVERIFY(arguments.at(1).toByteArray().startsWith("p=tls-exporter,,"));
}

void SaslChannelBindingUnitTest::scramSha256PlusClientAfterParams()
{
    const QString provider = QStringLiteral("qca-cyrus-sasl");
    if (!QCA::isSupported("sasl", provider))
        QSKIP("qca-cyrus-sasl is not available");

    QCA::SASL sasl(nullptr, provider);
    if (!sasl.supportsChannelBinding())
        QSKIP("The installed Cyrus SASL does not provide SASL_CHANNEL_BINDING");

    const QByteArray channelBindingData(32, '\x5a');
    QVERIFY(sasl.setChannelBinding(QStringLiteral("tls-exporter"), channelBindingData, true));

    QSignalSpy paramsSpy(&sasl, &QCA::SASL::needParams);
    QSignalSpy startedSpy(&sasl, &QCA::SASL::clientStarted);
    QSignalSpy errorSpy(&sasl, &QCA::SASL::error);

    connect(&sasl, &QCA::SASL::needParams, &sasl, [&sasl](const QCA::SASL::Params &) {
        sasl.setUsername(QStringLiteral("user"));
        sasl.setPassword(QCA::SecureArray("pencil"));
        sasl.continueAfterParams();
    });

    sasl.startClient(
        QStringLiteral("xmpp"), QStringLiteral("example.test"), QStringList {QStringLiteral("SCRAM-SHA-256-PLUS")});

    QTRY_VERIFY_WITH_TIMEOUT(!paramsSpy.isEmpty() || !errorSpy.isEmpty(), 5000);
    if (!errorSpy.isEmpty() && sasl.authCondition() == QCA::SASL::NoMechanism)
        QSKIP("The Cyrus SASL SCRAM-SHA-256 plugin is not installed");

    QCOMPARE(paramsSpy.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!startedSpy.isEmpty() || !errorSpy.isEmpty(), 5000);
    QVERIFY2(errorSpy.isEmpty(), "Unable to start SCRAM-SHA-256-PLUS after supplying parameters");
    QCOMPARE(startedSpy.size(), 1);
    QCOMPARE(sasl.mechanism(), QStringLiteral("SCRAM-SHA-256-PLUS"));

    const QList<QVariant> arguments = startedSpy.constFirst();
    QVERIFY(arguments.at(0).toBool());
    QVERIFY(arguments.at(1).toByteArray().startsWith("p=tls-exporter,,"));
}

void SaslChannelBindingUnitTest::completeClient_data()
{
    QTest::addColumn<QString>("hash");
    QTest::addColumn<QString>("mechanism");
    QTest::addColumn<bool>("lateParams");
    QTest::addColumn<bool>("badSignature");
    QTest::addColumn<QString>("provider");
    QStringList providers {QStringLiteral("qca-cyrus-sasl")};
#ifdef Q_OS_WIN
    providers += QStringLiteral("qca-wingss");
#endif
    for (const QString &provider : providers) {
        const QByteArray prefix = provider == QStringLiteral("qca-wingss") ? QByteArray("wingss-") : QByteArray();
        const auto       row =
            [&](const char *name, const char *hash, const char *mechanism, bool late = false, bool bad = false) {
                const QByteArray rowName = prefix + name;
                QTest::newRow(rowName.constData())
                    << QString::fromLatin1(hash) << QString::fromLatin1(mechanism) << late << bad << provider;
            };
        row("sha1-plus", "sha1", "SCRAM-SHA-1-PLUS");
        row("sha224-plus", "sha224", "SCRAM-SHA-224-PLUS");
        row("sha256-plus", "sha256", "SCRAM-SHA-256-PLUS");
        row("sha384-plus", "sha384", "SCRAM-SHA-384-PLUS");
        row("sha512-plus", "sha512", "SCRAM-SHA-512-PLUS");
        row("sha256-plus-interactive", "sha256", "SCRAM-SHA-256-PLUS", true);
        row("sha256-plus-bad-signature", "sha256", "SCRAM-SHA-256-PLUS", false, true);
    }
}

void SaslChannelBindingUnitTest::completeClient()
{
    QFETCH(QString, hash);
    QFETCH(QString, mechanism);
    QFETCH(bool, lateParams);
    QFETCH(bool, badSignature);
    QFETCH(QString, provider);

    if (!QCA::isSupported("sasl", provider))
        QSKIP("The selected SASL provider is not available");
    const QString macName = QStringLiteral("hmac(%1)").arg(hash);
    if (!QCA::isSupported(QStringList {hash, macName, QStringLiteral("pbkdf2(%1)").arg(hash)}))
        QSKIP("The reference hash, HMAC and PBKDF2 operations are unavailable");

    QCA::SASL sasl(nullptr, provider);
    if (!sasl.supportsChannelBinding())
        QSKIP("The installed Cyrus SASL does not provide channel binding");
    const QByteArray binding(32, '\x5a');
    QVERIFY(sasl.setChannelBinding(QStringLiteral("tls-exporter"), binding, true));
    auto credentials = [&sasl] {
        sasl.setUsername(QStringLiteral("user"));
        sasl.setPassword(QCA::SecureArray("pencil"));
    };
    if (lateParams)
        connect(&sasl, &QCA::SASL::needParams, &sasl, [&](const QCA::SASL::Params &) {
            credentials();
            sasl.continueAfterParams();
        });
    else
        credentials();

    QSignalSpy  started(&sasl, &QCA::SASL::clientStarted);
    QSignalSpy  steps(&sasl, &QCA::SASL::nextStep);
    QSignalSpy  authenticated(&sasl, &QCA::SASL::authenticated);
    QSignalSpy  errors(&sasl, &QCA::SASL::error);
    QStringList offered {mechanism};
    if (provider == QStringLiteral("qca-wingss"))
        offered.prepend(QStringLiteral("GSSAPI"));
    sasl.startClient(QStringLiteral("test"), QStringLiteral("example.test"), offered);
    QTRY_VERIFY_WITH_TIMEOUT(!started.isEmpty() || !errors.isEmpty(), 5000);
    QVERIFY2(errors.isEmpty(), "Unable to start the SCRAM client");
    QCOMPARE(sasl.mechanism(), mechanism);

    const QByteArray first = started.constFirst().at(1).toByteArray();
    const QByteArray header("p=tls-exporter,,");
    QVERIFY(first.startsWith(header));
    const int nonceAt = first.indexOf(",r=");
    QVERIFY(nonceAt >= header.size());
    const QByteArray nonce     = first.mid(nonceAt + 3) + QByteArray("server-nonce");
    const QByteArray salt      = QByteArray::fromHex("4125c247e4ab1e93e9524145");
    const QByteArray challenge = QByteArray("r=") + nonce + ",s=" + salt.toBase64() + ",i=4096";
    sasl.putStep(challenge);
    QTRY_VERIFY_WITH_TIMEOUT(!steps.isEmpty() || !errors.isEmpty(), 5000);
    QVERIFY2(errors.isEmpty(), "Unable to compute the SCRAM client proof");
    const QByteArray final   = steps.constFirst().at(0).toByteArray();
    const int        proofAt = final.lastIndexOf(",p=");
    QVERIFY(proofAt > 0);
    const QByteArray withoutProof = final.left(proofAt);
    const int        bindingEnd   = withoutProof.indexOf(',');
    QCOMPARE(QByteArray::fromBase64(withoutProof.mid(2, bindingEnd - 2)), header + binding);
    const QByteArray authMessage = first.mid(header.size()) + ',' + challenge + ',' + withoutProof;

    QCA::Hash               referenceHash(hash);
    const unsigned          keyLength = unsigned(referenceHash.process(QCA::SecureArray()).size());
    QCA::PBKDF2             kdf(hash);
    const QCA::SymmetricKey salted =
        kdf.makeKey(QCA::SecureArray("pencil"), QCA::InitializationVector(salt), keyLength, 4096u);
    QCOMPARE(salted.size(), int(keyLength));
    auto mac = [&](const QCA::SecureArray &key, const QCA::MemoryRegion &input) {
        return QCA::SecureArray(QCA::MessageAuthenticationCode(macName, QCA::SymmetricKey(key)).process(input));
    };
    const QCA::SecureArray clientKey = mac(salted, QCA::SecureArray("Client Key"));
    const QCA::SecureArray storedKey = QCA::Hash(hash).process(clientKey);
    const QCA::SecureArray signature = mac(storedKey, authMessage);
    QCA::SecureArray       proof(int(keyLength), 0);
    for (int i = 0; i < int(keyLength); ++i)
        proof[i] = clientKey[i] ^ signature[i];
    QCOMPARE(final.mid(proofAt + 3), proof.toByteArray().toBase64());

    const QCA::SecureArray serverKey = mac(salted, QCA::SecureArray("Server Key"));
    QByteArray             verifier  = mac(serverKey, authMessage).toByteArray().toBase64();
    if (badSignature)
        verifier[0] = verifier[0] == 'a' ? 'b' : 'a';
    sasl.putStep(QByteArray("v=") + verifier);
    QTRY_VERIFY_WITH_TIMEOUT(!authenticated.isEmpty() || !errors.isEmpty(), 5000);
    if (badSignature) {
        QVERIFY(authenticated.isEmpty());
        QCOMPARE(sasl.authCondition(), QCA::SASL::BadAuth);
    } else {
        QVERIFY(errors.isEmpty());
        QCOMPARE(authenticated.size(), 1);
    }
}

QTEST_MAIN(SaslChannelBindingUnitTest)

#include "saslchannelbindingunittest.moc"
