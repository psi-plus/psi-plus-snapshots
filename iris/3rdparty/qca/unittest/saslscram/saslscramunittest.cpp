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

class SaslScramUnitTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void scramSha256RoundTrip();

private:
    QCA::Initializer *m_init = nullptr;
};

void SaslScramUnitTest::initTestCase()
{
    m_init = new QCA::Initializer;
}

void SaslScramUnitTest::cleanupTestCase()
{
    delete m_init;
    m_init = nullptr;
}

void SaslScramUnitTest::scramSha256RoundTrip()
{
    if (qEnvironmentVariableIntValue("QCA_SASL_TEST_ENABLE") != 1)
        QSKIP("Set QCA_SASL_TEST_ENABLE=1 with the qca Cyrus SASL test account configured");

    const QString provider = QStringLiteral("qca-cyrus-sasl");
    QVERIFY2(QCA::isSupported("sasl", provider), "qca-cyrus-sasl is not available");

    const QString username = QStringLiteral("qca-scram-user");
    const QString password = QStringLiteral("pencil");
    const QString realm    = QStringLiteral("example.test");
    const QString service  = QStringLiteral("xmpp");
    const QString host     = realm;
    const QString mech     = QStringLiteral("SCRAM-SHA-256");

    QCA::SASL server(nullptr, provider);
    QCA::SASL client(nullptr, provider);

    QSignalSpy serverStartedSpy(&server, &QCA::SASL::serverStarted);
    QSignalSpy clientStartedSpy(&client, &QCA::SASL::clientStarted);
    QSignalSpy serverAuthSpy(&server, &QCA::SASL::authenticated);
    QSignalSpy clientAuthSpy(&client, &QCA::SASL::authenticated);
    QSignalSpy serverErrorSpy(&server, &QCA::SASL::error);
    QSignalSpy clientErrorSpy(&client, &QCA::SASL::error);

    QString     authenticatedUser;
    QStringList trace;
    connect(&server, &QCA::SASL::serverStarted, &server, [&] {
        trace += QStringLiteral("serverStarted:%1").arg(server.mechanismList().join(QLatin1Char(',')));
    });
    connect(&server, &QCA::SASL::authCheck, &server, [&](const QString &user, const QString &) {
        authenticatedUser = user;
        trace += QStringLiteral("serverAuthCheck:%1").arg(user);
        server.continueAfterAuthCheck();
    });
    connect(&server, &QCA::SASL::nextStep, &client, [&](const QByteArray &stepData) {
        trace += QStringLiteral("serverNext:%1:%2").arg(stepData.size()).arg(QString::fromLatin1(stepData.toBase64()));
        client.putStep(stepData);
    });
    connect(&client, &QCA::SASL::nextStep, &server, [&](const QByteArray &stepData) {
        trace += QStringLiteral("clientNext:%1:%2").arg(stepData.size()).arg(QString::fromLatin1(stepData.toBase64()));
        server.putStep(stepData);
    });
    connect(&server, &QCA::SASL::authenticated, &server, [&] { trace += QStringLiteral("serverAuthenticated"); });
    connect(&client, &QCA::SASL::authenticated, &client, [&] { trace += QStringLiteral("clientAuthenticated"); });
    connect(&server, &QCA::SASL::error, &server, [&] {
        trace += QStringLiteral("serverError:%1:%2").arg(int(server.errorCode())).arg(int(server.authCondition()));
    });
    connect(&client, &QCA::SASL::error, &client, [&] {
        trace += QStringLiteral("clientError:%1:%2").arg(int(client.errorCode())).arg(int(client.authCondition()));
    });
    connect(&client, &QCA::SASL::clientStarted, &server, [&](bool haveClientInit, const QByteArray &clientInitData) {
        trace += QStringLiteral("clientStarted:%1:%2")
                     .arg(haveClientInit ? 1 : 0)
                     .arg(QString::fromLatin1(clientInitData.toBase64()));
        if (haveClientInit)
            server.putServerFirstStep(mech, clientInitData);
        else
            server.putServerFirstStep(mech);
    });

    server.startServer(service, host, realm, QCA::SASL::AllowServerSendLast);
    QTRY_VERIFY_WITH_TIMEOUT(!serverStartedSpy.isEmpty() || !serverErrorSpy.isEmpty(), 5000);
    QVERIFY2(serverErrorSpy.isEmpty(), "Unable to initialize the qca-cyrus-sasl server");
    QVERIFY2(server.mechanismList().contains(mech),
             qPrintable(QStringLiteral("Ubuntu Cyrus SASL server does not advertise %1: %2")
                            .arg(mech, server.mechanismList().join(QLatin1Char(' ')))));

    client.setUsername(username);
    client.setPassword(QCA::SecureArray(password.toUtf8()));
    client.setRealm(realm);
    client.startClient(service, host, QStringList {mech});

    QTRY_VERIFY_WITH_TIMEOUT((!clientAuthSpy.isEmpty() && !serverAuthSpy.isEmpty()) || !clientErrorSpy.isEmpty() ||
                                 !serverErrorSpy.isEmpty(),
                             10000);

    const QByteArray clientError = QStringLiteral("Client SASL error code=%1 authCondition=%2")
                                       .arg(int(client.errorCode()))
                                       .arg(int(client.authCondition()))
                                       .toUtf8();
    const QByteArray serverError = QStringLiteral("Server SASL error code=%1 authCondition=%2")
                                       .arg(int(server.errorCode()))
                                       .arg(int(server.authCondition()))
                                       .toUtf8();

    const QByteArray traceText = trace.join(QStringLiteral(" | ")).toUtf8();
    if (!clientErrorSpy.isEmpty())
        qWarning().noquote() << clientError << traceText;
    if (!serverErrorSpy.isEmpty())
        qWarning().noquote() << serverError << traceText;

    QVERIFY2(clientErrorSpy.isEmpty(), traceText.constData());
    QVERIFY2(serverErrorSpy.isEmpty(), traceText.constData());
    QCOMPARE(clientStartedSpy.size(), 1);
    QCOMPARE(client.mechanism(), mech);
    QCOMPARE(clientAuthSpy.size(), 1);
    QCOMPARE(serverAuthSpy.size(), 1);
    QVERIFY2(authenticatedUser.startsWith(username),
             qPrintable(QStringLiteral("Unexpected authenticated user: %1").arg(authenticatedUser)));
}

QTEST_MAIN(SaslScramUnitTest)

#include "saslscramunittest.moc"
