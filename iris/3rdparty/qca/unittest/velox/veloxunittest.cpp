/**
 * Copyright (C) 2006 Brad Hards <bradh@frogmouth.net>
 * Copyright (C) 2026 QCA contributors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QtCrypto>

#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif

namespace {

const QString qcaOsslProvider = QStringLiteral("qca-ossl");

QStringList testHostNames()
{
    return {
        QStringLiteral("alice.sni.qca.test"),
        QStringLiteral("bob.sni.qca.test"),
        QStringLiteral("carol.sni.qca.test"),
        QStringLiteral("dave.sni.qca.test"),
        QStringLiteral("mallory.sni.qca.test"),
        QStringLiteral("ivan.sni.qca.test"),
    };
}

QString certificateFileName(const QString &hostName)
{
    return QStringLiteral("velox-certs/%1.crt").arg(hostName.section(QLatin1Char('.'), 0, 0));
}

QString privateKeyFileName(const QString &hostName)
{
    return QStringLiteral("velox-certs/%1.key").arg(hostName.section(QLatin1Char('.'), 0, 0));
}

struct ServerIdentity
{
    QCA::Certificate certificate;
    QCA::PrivateKey  privateKey;

    bool isValid() const
    {
        return !certificate.isNull() && !privateKey.isNull();
    }
};

} // namespace

class LocalSniServer;

class SniConnection : public QObject
{
    Q_OBJECT
public:
    SniConnection(QTcpSocket *socket, LocalSniServer *server);

    void stop();

private:
    void socketReadyRead();
    void socketDisconnected();
    void tlsHostNameReceived();
    void tlsHandshaken();
    void tlsReadyReadOutgoing();
    void tlsClosed();
    void tlsError();

private:
    QTcpSocket     *m_socket;
    QCA::TLS       *m_tls;
    LocalSniServer *m_server;
};

class LocalSniServer : public QObject
{
    Q_OBJECT
public:
    explicit LocalSniServer(QObject *parent = nullptr)
        : QObject(parent)
        , m_server(nullptr)
    {
    }

    ServerIdentity identityForHost(const QString &hostName) const
    {
        return m_identities.value(hostName, m_identities.value(QStringLiteral("default.sni.qca.test")));
    }

    void reportHostName(const QString &hostName)
    {
        emit serverNameReceived(hostName);
    }

    void reportConnectionError(const QString &message)
    {
        emit connectionError(message);
    }

    void reportChannelBinding(const QString &type, const QByteArray &data)
    {
        emit channelBindingReceived(type, data);
    }

public Q_SLOTS:
    void start()
    {
        QString errorMessage;
        if (!loadIdentities(&errorMessage)) {
            emit failed(errorMessage);
            return;
        }

        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &LocalSniServer::newConnection);

        if (!m_server->listen(QHostAddress::LocalHost, 0)) {
            emit failed(m_server->errorString());
            return;
        }

        emit started(m_server->serverPort());
    }

    void stop()
    {
        const QList<SniConnection *> connections = findChildren<SniConnection *>();
        for (SniConnection *connection : connections) {
            connection->stop();
            delete connection;
        }

        if (m_server) {
            m_server->close();
            delete m_server;
            m_server = nullptr;
        }

        // Release all provider-backed QCA objects while QCA::Initializer
        // and the qca-ossl plugin are still alive.
        m_identities.clear();
    }

Q_SIGNALS:
    void started(quint16 port);
    void failed(const QString &message);
    void serverNameReceived(const QString &hostName);
    void channelBindingReceived(const QString &type, const QByteArray &data);
    void connectionError(const QString &message);

private Q_SLOTS:
    void newConnection()
    {
        while (m_server && m_server->hasPendingConnections()) {
            QTcpSocket *socket = m_server->nextPendingConnection();
            new SniConnection(socket, this);
        }
    }

private:
    bool loadIdentity(const QString &hostName, QString *errorMessage)
    {
        QCA::ConvertResult certificateResult = QCA::ErrorDecode;
        QCA::ConvertResult keyResult         = QCA::ErrorDecode;

        ServerIdentity identity;
        identity.certificate =
            QCA::Certificate::fromPEMFile(certificateFileName(hostName), &certificateResult, qcaOsslProvider);
        identity.privateKey =
            QCA::PrivateKey::fromPEMFile(privateKeyFileName(hostName), QCA::SecureArray(), &keyResult, qcaOsslProvider);

        if (certificateResult != QCA::ConvertGood || keyResult != QCA::ConvertGood || !identity.isValid()) {
            *errorMessage = QStringLiteral("Unable to load the local TLS identity for %1").arg(hostName);
            return false;
        }

        m_identities.insert(hostName, identity);
        return true;
    }

    bool loadIdentities(QString *errorMessage)
    {
        QStringList hostNames = testHostNames();
        hostNames += QStringLiteral("default.sni.qca.test");

        for (const QString &hostName : hostNames) {
            if (!loadIdentity(hostName, errorMessage))
                return false;
        }
        return true;
    }

    QTcpServer                    *m_server;
    QHash<QString, ServerIdentity> m_identities;
};

SniConnection::SniConnection(QTcpSocket *socket, LocalSniServer *server)
    : QObject(server)
    , m_socket(socket)
    , m_tls(new QCA::TLS(QCA::TLS::Stream, this, qcaOsslProvider))
    , m_server(server)
{
    m_socket->setParent(this);

    connect(m_socket, &QTcpSocket::readyRead, this, &SniConnection::socketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &SniConnection::socketDisconnected);

    connect(m_tls, &QCA::TLS::hostNameReceived, this, &SniConnection::tlsHostNameReceived);
    connect(m_tls, &QCA::TLS::handshaken, this, &SniConnection::tlsHandshaken);
    connect(m_tls, &QCA::TLS::readyReadOutgoing, this, &SniConnection::tlsReadyReadOutgoing);
    connect(m_tls, &QCA::TLS::closed, this, &SniConnection::tlsClosed);
    connect(m_tls, &QCA::TLS::error, this, &SniConnection::tlsError);

    m_tls->startServer();

    if (m_socket->bytesAvailable() > 0)
        socketReadyRead();
}

void SniConnection::stop()
{
    if (m_socket)
        m_socket->abort();
}

void SniConnection::socketReadyRead()
{
    m_tls->writeIncoming(m_socket->readAll());
}

void SniConnection::socketDisconnected()
{
    deleteLater();
}

void SniConnection::tlsHostNameReceived()
{
    const QString hostName = m_tls->hostName();
    m_server->reportHostName(hostName);

    const ServerIdentity identity = m_server->identityForHost(hostName);
    if (!identity.isValid()) {
        m_server->reportConnectionError(QStringLiteral("No TLS identity is available for %1").arg(hostName));
        m_tls->continueAfterStep();
        return;
    }

    QCA::CertificateChain chain;
    chain.append(identity.certificate);
    m_tls->setCertificate(chain, identity.privateKey);
    m_tls->continueAfterStep();
}

void SniConnection::tlsHandshaken()
{
    const QString    type = m_tls->defaultChannelBindingType();
    const QByteArray data = m_tls->channelBinding(type);
    if (type.isEmpty() || data.isEmpty()) {
        m_server->reportConnectionError(QStringLiteral("No default TLS channel binding is available"));
    } else {
        m_server->reportChannelBinding(type, data);
    }
    m_tls->continueAfterStep();
}

void SniConnection::tlsReadyReadOutgoing()
{
    const QByteArray outgoing = m_tls->readOutgoing();
    if (!outgoing.isEmpty())
        m_socket->write(outgoing);
}

void SniConnection::tlsClosed()
{
    m_socket->disconnectFromHost();
}

void SniConnection::tlsError()
{
    m_server->reportConnectionError(
        QStringLiteral("Server-side TLS error %1").arg(static_cast<int>(m_tls->errorCode())));
    m_socket->abort();
}

class TlsTest : public QObject
{
public:
    explicit TlsTest(const QCA::Certificate &rootCertificate)
        : m_socket(new QTcpSocket(this))
        , m_tls(new QCA::TLS(QCA::TLS::Stream, this, qcaOsslProvider))
        , m_waitLoop(nullptr)
        , m_handshaken(false)
        , m_done(false)
        , m_identityResult(QCA::TLS::NoCertificate)
    {
        QCA::CertificateCollection trustedCertificates;
        trustedCertificates.addCertificate(rootCertificate);
        m_tls->setTrustedCertificates(trustedCertificates);

        connect(m_socket, &QTcpSocket::connected, this, &TlsTest::socketConnected);
        connect(m_socket, &QTcpSocket::readyRead, this, &TlsTest::socketReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &TlsTest::socketDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(m_socket, &QTcpSocket::errorOccurred, this, &TlsTest::socketError);
#else
        connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, &TlsTest::socketError);
#endif

        connect(m_tls, &QCA::TLS::handshaken, this, &TlsTest::tlsHandshaken);
        connect(m_tls, &QCA::TLS::readyReadOutgoing, this, &TlsTest::tlsReadyReadOutgoing);
        connect(m_tls, &QCA::TLS::error, this, &TlsTest::tlsError);
    }

    ~TlsTest() override
    {
        m_socket->abort();
    }

    void start(const QString &connectHost, quint16 port, const QString &tlsHostName)
    {
        m_tlsHostName = tlsHostName;
        m_socket->connectToHost(connectHost, port);
    }

    bool waitForHandshake(int timeout = 5000)
    {
        if (m_done)
            return m_handshaken;

        QEventLoop eventLoop;
        QTimer     timeoutTimer;
        timeoutTimer.setSingleShot(true);

        connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);

        m_waitLoop = &eventLoop;
        timeoutTimer.start(timeout);
        eventLoop.exec();
        m_waitLoop = nullptr;

        if (!m_done) {
            m_done        = true;
            m_errorString = QStringLiteral("Timed out while waiting for the local TLS handshake");
            m_socket->abort();
        }

        return m_handshaken;
    }

    QString errorString() const
    {
        return m_errorString;
    }

    QCA::TLS::IdentityResult identityResult() const
    {
        return m_identityResult;
    }

    QCA::Certificate peerCertificate() const
    {
        const QCA::CertificateChain chain = m_tls->peerCertificateChain();
        return chain.isEmpty() ? QCA::Certificate() : chain.primary();
    }

    QString channelBindingType() const
    {
        return m_channelBindingType;
    }

    QByteArray channelBindingData() const
    {
        return m_channelBindingData;
    }

private:
    void socketConnected()
    {
        m_tls->startClient(m_tlsHostName);
    }

    void socketReadyRead()
    {
        m_tls->writeIncoming(m_socket->readAll());
    }

    void socketDisconnected()
    {
        if (!m_done)
            finish(false, QStringLiteral("The local TLS server disconnected before the handshake completed"));
    }

    void socketError(QAbstractSocket::SocketError)
    {
        if (!m_done)
            finish(false, m_socket->errorString());
    }

    void tlsHandshaken()
    {
        m_identityResult     = m_tls->peerIdentityResult();
        m_channelBindingType = m_tls->defaultChannelBindingType();
        m_channelBindingData = m_tls->channelBinding(m_channelBindingType);
        m_handshaken         = true;
        m_tls->continueAfterStep();
        finish(true, QString());
    }

    void tlsReadyReadOutgoing()
    {
        const QByteArray outgoing = m_tls->readOutgoing();
        if (!outgoing.isEmpty())
            m_socket->write(outgoing);
    }

    void tlsError()
    {
        finish(false, QStringLiteral("Client-side TLS error %1").arg(static_cast<int>(m_tls->errorCode())));
    }

private:
    void finish(bool success, const QString &errorString)
    {
        if (m_done)
            return;

        m_done = true;
        if (!success)
            m_errorString = errorString;
        if (m_waitLoop)
            m_waitLoop->quit();
    }

    QTcpSocket              *m_socket;
    QCA::TLS                *m_tls;
    QEventLoop              *m_waitLoop;
    QString                  m_tlsHostName;
    QString                  m_channelBindingType;
    QByteArray               m_channelBindingData;
    bool                     m_handshaken;
    bool                     m_done;
    QString                  m_errorString;
    QCA::TLS::IdentityResult m_identityResult;
};

class VeloxUnitTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void sni_data();
    void sni();

private:
    QCA::Initializer *m_init         = nullptr;
    LocalSniServer   *m_server       = nullptr;
    QThread          *m_serverThread = nullptr;
    quint16           m_serverPort   = 0;
    QCA::Certificate  m_rootCertificate;
};

void VeloxUnitTest::initTestCase()
{
    m_init = new QCA::Initializer;

    QVERIFY2(QCA::isSupported("tls", qcaOsslProvider),
             qPrintable(QStringLiteral("TLS is not available from qca-ossl.\n%1").arg(QCA::pluginDiagnosticText())));

    QCA::ConvertResult rootResult = QCA::ErrorDecode;
    m_rootCertificate =
        QCA::Certificate::fromPEMFile(QStringLiteral("velox-certs/root.crt"), &rootResult, qcaOsslProvider);
    QCOMPARE(rootResult, QCA::ConvertGood);
    QVERIFY(!m_rootCertificate.isNull());

    m_server       = new LocalSniServer;
    m_serverThread = new QThread;
    m_server->moveToThread(m_serverThread);

    connect(m_serverThread, &QThread::started, m_server, &LocalSniServer::start);
    connect(m_serverThread, &QThread::finished, m_server, &QObject::deleteLater);

    QSignalSpy startedSpy(m_server, &LocalSniServer::started);
    QSignalSpy failedSpy(m_server, &LocalSniServer::failed);

    m_serverThread->start();

    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() > 0 || failedSpy.count() > 0, 5000);
    if (!failedSpy.isEmpty())
        QFAIL(qPrintable(failedSpy.constFirst().constFirst().toString()));

    QCOMPARE(startedSpy.count(), 1);
    m_serverPort = startedSpy.constFirst().constFirst().toUInt();
    QVERIFY(m_serverPort != 0);
}

void VeloxUnitTest::cleanupTestCase()
{
    if (m_server && m_serverThread && m_serverThread->isRunning()) {
        QMetaObject::invokeMethod(m_server, &LocalSniServer::stop, Qt::BlockingQueuedConnection);
        m_serverThread->quit();
        m_serverThread->wait();
    }

    delete m_serverThread;
    m_serverThread = nullptr;
    m_server       = nullptr;

    // m_rootCertificate owns a provider context. It must be destroyed
    // before QCA::Initializer unloads qca-ossl.
    m_rootCertificate = QCA::Certificate();

    delete m_init;
    m_init = nullptr;
}

void VeloxUnitTest::sni_data()
{
    QTest::addColumn<QString>("hostName");

    const QStringList hostNames = testHostNames();
    for (const QString &hostName : hostNames) {
        const QByteArray rowName = hostName.section(QLatin1Char('.'), 0, 0).toLatin1();
        QTest::newRow(rowName.constData()) << hostName;
    }
}

void VeloxUnitTest::sni()
{
    QFETCH(QString, hostName);

    QSignalSpy serverNameSpy(m_server, &LocalSniServer::serverNameReceived);
    QSignalSpy channelBindingSpy(m_server, &LocalSniServer::channelBindingReceived);
    QSignalSpy serverErrorSpy(m_server, &LocalSniServer::connectionError);

    TlsTest client(m_rootCertificate);
    client.start(QHostAddress(QHostAddress::LocalHost).toString(), m_serverPort, hostName);

    QVERIFY2(client.waitForHandshake(), qPrintable(client.errorString()));
    QCOMPARE(client.identityResult(), QCA::TLS::Valid);

    const QCA::Certificate peerCertificate = client.peerCertificate();
    QVERIFY(!peerCertificate.isNull());
    QVERIFY(peerCertificate.matchesHostName(hostName));

    QCA::ConvertResult     expectedResult = QCA::ErrorDecode;
    const QCA::Certificate expectedCertificate =
        QCA::Certificate::fromPEMFile(certificateFileName(hostName), &expectedResult, qcaOsslProvider);
    QCOMPARE(expectedResult, QCA::ConvertGood);
    QVERIFY(peerCertificate == expectedCertificate);

    QTRY_COMPARE_WITH_TIMEOUT(serverNameSpy.count(), 1, 1000);
    QCOMPARE(serverNameSpy.constFirst().constFirst().toString(), hostName);

    QVERIFY(!client.channelBindingType().isEmpty());
    QVERIFY(!client.channelBindingData().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(channelBindingSpy.count(), 1, 1000);
    QCOMPARE(channelBindingSpy.constFirst().at(0).toString(), client.channelBindingType());
    QCOMPARE(channelBindingSpy.constFirst().at(1).toByteArray(), client.channelBindingData());
    QVERIFY2(serverErrorSpy.isEmpty(),
             serverErrorSpy.isEmpty() ? "" : qPrintable(serverErrorSpy.constFirst().constFirst().toString()));
}

QTEST_MAIN(VeloxUnitTest)

#include "veloxunittest.moc"
