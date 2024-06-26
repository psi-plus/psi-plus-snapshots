/*
 * bsocket.cpp - QSocket wrapper based on Bytestream with SRV DNS support
 * Copyright (C) 2003  Justin Karneges
 * Copyright (C) 2009-2010  Dennis Schridde
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

#include "bsocket.h"

#include "netnames.h"

#include <QHostAddress>
#include <QMetaType>
#include <QTcpSocket>
#include <QTimer>

#include <optional>

// #include <limits>  // if it's still needed please comment why

#define BSDEBUG (qDebug() << this << "#" << __FUNCTION__ << ":")
static std::optional<bool> enable_logs;
#define BSLOG(msg)                                                                                                     \
    {                                                                                                                  \
        if (!enable_logs.has_value()) {                                                                                \
            enable_logs = qgetenv("BS_DEBUG") == "1";                                                                  \
        }                                                                                                              \
        if (*enable_logs) {                                                                                            \
            msg;                                                                                                       \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)                                                                                                      \
        ;

#define READBUFSIZE 65536

// CS_NAMESPACE_BEGIN
class QTcpSocketSignalRelay : public QObject {
    Q_OBJECT
public:
    QTcpSocketSignalRelay(QTcpSocket *sock, QObject *parent = nullptr) : QObject(parent)
    {
        qRegisterMetaType<QAbstractSocket::SocketError>("QAbstractSocket::SocketError");
        connect(sock, &QTcpSocket::hostFound, this, &QTcpSocketSignalRelay::sock_hostFound, Qt::QueuedConnection);
        connect(sock, &QTcpSocket::connected, this, &QTcpSocketSignalRelay::sock_connected, Qt::QueuedConnection);
        connect(sock, &QTcpSocket::disconnected, this, &QTcpSocketSignalRelay::sock_disconnected, Qt::QueuedConnection);
        connect(sock, &QTcpSocket::readyRead, this, &QTcpSocketSignalRelay::sock_readyRead, Qt::QueuedConnection);
        connect(sock, &QTcpSocket::bytesWritten, this, &QTcpSocketSignalRelay::sock_bytesWritten, Qt::QueuedConnection);
        connect(sock, &QTcpSocket::errorOccurred, this, &QTcpSocketSignalRelay::sock_error, Qt::QueuedConnection);
    }

signals:
    void hostFound();
    void connected();
    void disconnected();
    void readyRead();
    void bytesWritten(qint64);
    void error(QAbstractSocket::SocketError);

public slots:
    void sock_hostFound() { emit hostFound(); }

    void sock_connected() { emit connected(); }

    void sock_disconnected() { emit disconnected(); }

    void sock_readyRead() { emit readyRead(); }

    void sock_bytesWritten(qint64 x) { emit bytesWritten(x); }

    void sock_error(QAbstractSocket::SocketError x) { emit error(x); }
};

class HappyEyeballsConnector : public QObject {
    Q_OBJECT
public:
    enum State { Failure, Created, Resolve, Connecting, Connected };

    struct SockData {
        QTcpSocket            *sock;
        QTcpSocketSignalRelay *relay;
        State                  state;
        QString                hostname; // last resolved name
        QString                service;  // one of services passed to service (SRV) resolver
        XMPP::ServiceResolver *resolver;
    };

    /*! source data */
    // QString                               service;
    QString                               transport;
    QString                               domain;
    quint16                               port = 0;
    QHostAddress                          address;
    QAbstractSocket::NetworkLayerProtocol fallbackProtocol = QAbstractSocket::IPv4Protocol;

    /*! runtime data */
    QString         lastError;
    int             lastIndex;
    QList<SockData> sockets;
    QTimer          fallbackTimer;

    HappyEyeballsConnector(QObject *parent) : QObject(parent)
    {
        fallbackTimer.setSingleShot(true);
        fallbackTimer.setInterval(250); /* rfc recommends 150-250ms */
        connect(&fallbackTimer, SIGNAL(timeout()), SLOT(startFallback()));
    }

    SockData &addSocket()
    {
        SockData sd;
        sd.state = Created;
        sd.sock  = new QTcpSocket(this);
        sd.sock->setProxy(QNetworkProxy::NoProxy);
        sd.sock->setReadBufferSize(READBUFSIZE);
        sd.relay    = new QTcpSocketSignalRelay(sd.sock, this);
        sd.resolver = nullptr;
        connect(sd.relay, &QTcpSocketSignalRelay::connected, this, &HappyEyeballsConnector::qs_connected);
        connect(sd.relay, &QTcpSocketSignalRelay::error, this, &HappyEyeballsConnector::qs_error);
        sockets.append(sd);
        return sockets[sockets.count() - 1];
    }

    void cleanup()
    {
        for (int i = 0; i < sockets.count(); i++) {
            abortSocket(sockets[i]);
        }
        fallbackTimer.stop();
    }

    void connectToHost(const QHostAddress &address, quint16 port)
    {
        BSLOG(BSDEBUG << "a:" << address << "p:" << port);
        this->address = address;
        SockData &sd  = addSocket();
        sd.state      = Connecting;
        sd.sock->connectToHost(address, port);
    }

    /* Connect to a host via the specified protocol, or the default protocols if not specified */
    void connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol)
    {
        BSLOG(BSDEBUG << "h:" << host << "p:" << port << "pr:" << protocol);
        this->domain = host;
        this->port   = port;
        SockData &sd = addSocket();

        QHostAddress addr(host);
        if (addr.isNull()) {
            sd.resolver = new XMPP::ServiceResolver;
            initResolver(sd.resolver);
            sd.resolver->setProtocol(protocol == QAbstractSocket::UnknownNetworkLayerProtocol
                                         ? (fallbackProtocol == QAbstractSocket::IPv4Protocol
                                                ? XMPP::ServiceResolver::IPv6
                                                : XMPP::ServiceResolver::IPv4)
                                         : (protocol == QAbstractSocket::IPv4Protocol ? XMPP::ServiceResolver::IPv4
                                                                                      : XMPP::ServiceResolver::IPv6));
            if (protocol == QAbstractSocket::UnknownNetworkLayerProtocol) {
                addSocket();
                fallbackTimer.start();
            }
            sd.state = Resolve;
            sd.resolver->start(domain, port);
        } else {
            // connecting by IP.
            lastIndex = sockets.count() - 1;
            sd.state  = Connecting;
            sd.sock->connectToHost(addr, port);
        }
    }

    void connectToHost(const QStringList &services, const QString &transport, const QString &domain, quint16 port)
    {
        BSLOG(BSDEBUG << "s:" << services << "t:" << transport << "d:" << domain);
        // this->service   = service;
        this->transport = transport;
        this->domain    = domain;
        this->port      = port;
        SockData &sd    = addSocket();
        sd.resolver     = new XMPP::ServiceResolver(this);
        sd.resolver->setProtocol(XMPP::ServiceResolver::HappyEyeballs);
        connect(sd.resolver, &XMPP::ServiceResolver::srvReady, this, &HappyEyeballsConnector::splitSrvResolvers);
        // we don't care about special handling of fail. we have fallback host there anyway
        connect(sd.resolver, &XMPP::ServiceResolver::srvFailed, this, &HappyEyeballsConnector::splitSrvResolvers);
        sd.state = Resolve;
        sd.resolver->start(services, transport, domain, port);
    }

    SockData takeCurrent(QObject *parent)
    {
        SockData csd = sockets.takeAt(lastIndex);
        lastIndex    = -1;
        disconnect(csd.relay);
        csd.relay->setParent(parent);
        csd.sock->setParent(parent);
        delete csd
            .resolver; // FIME ensure it's accessible only from connected signal. we don't delete resolver from its slot
        csd.resolver = nullptr;
        return csd;
    }

private:
    void abortSocket(SockData &sd)
    {
        sd.relay->disconnect(this);
        if (sd.state >= Connecting) {
            sd.sock->abort();
        }
        if (sd.resolver) {
            sd.resolver->stop();
            disconnect(sd.resolver);
            sd.resolver->deleteLater(); // or just delete ?
        }
        delete sd.relay;
        delete sd.sock;
    }

    void initResolver(XMPP::ServiceResolver *resolver)
    {
        resolver->setParent(this);
        connect(resolver, &XMPP::ServiceResolver::resultReady, this, &HappyEyeballsConnector::handleDnsReady);
        connect(resolver, &XMPP::ServiceResolver::error, this, &HappyEyeballsConnector::handleDnsError);
    }

    void setCurrentByResolver(XMPP::ServiceResolver *resolver)
    {
        for (int i = 0; i < sockets.count(); i++) {
            if (sockets.at(i).resolver == resolver) {
                lastIndex = i;
                return;
            }
        }
        lastIndex = -1;
    }

    void setCurrentByRelay(QTcpSocketSignalRelay *relay)
    {
        for (int i = 0; i < sockets.count(); i++) {
            if (sockets.at(i).relay == relay) {
                lastIndex = i;
                return;
            }
        }
        lastIndex = -1;
    }

private slots:
    /*
    Notice: most probably recipient should reparent socket and relay
    */
    void qs_connected()
    {
        BSLOG(BSDEBUG);
        QPointer<HappyEyeballsConnector> valid(this);
        setCurrentByRelay(static_cast<QTcpSocketSignalRelay *>(sender()));
        for (int i = 0; i < sockets.count(); i++) {
            if (i != lastIndex) {
                abortSocket(sockets[i]);
            } else {
                disconnect(sockets[i].relay);
                sockets[i].state = Connected;
            }
            emit connected();
            if (!valid)
                return;
        }
    }

    void qs_error(QAbstractSocket::SocketError errorCode)
    {
        setCurrentByRelay(static_cast<QTcpSocketSignalRelay *>(sender()));
        // TODO remember error code
        lastError = sockets[lastIndex].sock->errorString();
        BSLOG(BSDEBUG << "error:" << lastError);

        if (sockets[lastIndex].resolver) {
            sockets[lastIndex].sock->abort();
            sockets[lastIndex].state = Resolve;
            sockets[lastIndex].resolver->tryNext();
        } else {
            // it seems we connect by hostaddress. just one socket w/o resolver
            emit error(errorCode);
        }
    }

    void splitSrvResolvers()
    {
        BSLOG(BSDEBUG << "splitting resolvers");
        setCurrentByResolver(static_cast<XMPP::ServiceResolver *>(sender()));
        Q_ASSERT(lastIndex >= 0);

        auto      tmp  = lastIndex;
        SockData &sdv6 = addSocket();
        SockData &sdv4 = sockets[tmp];

        XMPP::ServiceResolver::ProtoSplit ps = sdv4.resolver->happySplit();
        initResolver(ps.ipv4);
        initResolver(ps.ipv6);

        disconnect(sdv4.resolver);
        sdv4.resolver->deleteLater();

        sdv4.resolver = ps.ipv4;
        sdv4.state    = Created;
        sdv6.resolver = ps.ipv6;

        if (fallbackProtocol == QAbstractSocket::IPv4Protocol) {
            sdv6.state = Resolve;
            sdv6.resolver->tryNext();
        } else {
            sdv4.state = Resolve;
            sdv4.resolver->tryNext();
        }
        fallbackTimer.start();
    }

    /* host resolved, now try to connect to it */
    void handleDnsReady(const QHostAddress &address, quint16 port, const QString &hostname, const QString &service)
    {
        BSLOG(BSDEBUG << "a:" << address << "p:" << port);
        setCurrentByResolver(static_cast<XMPP::ServiceResolver *>(sender()));
        sockets[lastIndex].state    = Connecting;
        sockets[lastIndex].hostname = hostname;
        sockets[lastIndex].service  = service;
        sockets[lastIndex].sock->connectToHost(address, port);
    }

    /* resolver failed the dns lookup */
    void handleDnsError(XMPP::ServiceResolver::Error e)
    {
        BSLOG(BSDEBUG << "e:" << e);
        if (!fallbackTimer.isActive()) {
            emit error(QAbstractSocket::HostNotFoundError);
        }
    }

    void startFallback()
    {
        BSLOG(BSDEBUG);
        for (int i = 0; i < sockets.count(); i++) {
            SockData &sd = sockets[i];
            if (sd.state == Created) {
                sd.state = Resolve;
                if (sd.resolver) {
                    sd.resolver->tryNext();
                } else {
                    sd.resolver = new XMPP::ServiceResolver;
                    initResolver(sd.resolver);
                    sd.resolver->setProtocol(fallbackProtocol == QAbstractSocket::IPv4Protocol
                                                 ? XMPP::ServiceResolver::IPv4
                                                 : XMPP::ServiceResolver::IPv6);
                    sd.resolver->start(domain, port);
                }
            }
        }
    }

signals:
    void connected();
    void error(QAbstractSocket::SocketError);
};

class BSocket::Private {
public:
    Private()
    {
        qsock       = nullptr;
        qsock_relay = nullptr;
    }

    QTcpSocket            *qsock;
    QTcpSocketSignalRelay *qsock_relay;
    int                    state;

    QString      service; //!< One of passed to BSocket::connectToHost(QList<QString>)
    QString      domain;  //!< Domain we are currently connected to
    QString      host;    //!< Hostname we are currently connected to
    QHostAddress address; //!< IP address we are currently connected to
    quint16      port;    //!< Port we are currently connected to

    QPointer<HappyEyeballsConnector> connector;
};

BSocket::BSocket(QObject *parent) : ByteStream(parent)
{
    d = new Private;
    resetConnection();
}

BSocket::~BSocket()
{
    resetConnection(true);
    delete d;
}

void BSocket::resetConnection(bool clear)
{
    BSLOG(BSDEBUG << clear);
    if (d->connector) {
        d->connector->deleteLater();
        disconnect(d->connector);
        d->connector = nullptr;
    }

    if (d->qsock) {
        delete d->qsock_relay;
        d->qsock_relay = nullptr;

        // move remaining into the local queue
        if (d->qsock->isOpen()) {
            QByteArray block(int(d->qsock->bytesAvailable()),
                             0); // memory won't never be cheap enough to have gigabytes for socket buffer
            if (block.size()) {
                d->qsock->read(block.data(), block.size());
                appendRead(block);
            }
            d->qsock->close();
        }

        // d->sd.deleteLater(d->qsock);
        d->qsock->deleteLater();
        d->qsock = nullptr;
    } else {
        if (clear)
            clearReadBuffer();
    }

    d->state   = Idle;
    d->domain  = "";
    d->host    = "";
    d->address = QHostAddress();
    d->port    = 0;
    setOpenMode(QIODevice::NotOpen);
}

void BSocket::ensureConnector()
{
    if (!d->connector) {
        d->connector = new HappyEyeballsConnector(this);
        connect(d->connector, &HappyEyeballsConnector::connected, this, &BSocket::qs_connected);
        connect(d->connector, &HappyEyeballsConnector::error, this, &BSocket::qs_error);
    }
}

/* Connect to an already resolved host */
void BSocket::connectToHost(const QHostAddress &address, quint16 port)
{
    BSLOG(BSDEBUG << address << port);
    resetConnection(true);
    d->address = address;
    d->port    = port;
    d->state   = Connecting;

    ensureConnector();
    d->connector->connectToHost(address, port);
}

/* Connect to a host via the specified protocol, or the default protocols if not specified */
void BSocket::connectToHost(const QString &host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol)
{
    BSLOG(BSDEBUG << host << port << protocol);
    resetConnection(true);
    d->host  = host;
    d->port  = port;
    d->state = Connecting;

    ensureConnector();
    d->connector->connectToHost(host, port, protocol);
}

/* Connect to the hosts for the specified services */
void BSocket::connectToHost(const QStringList &services, const QString &transport, const QString &domain, quint16 port)
{
    BSLOG(BSDEBUG << services << transport << domain << port);
    resetConnection(true);
    d->domain = domain;
    d->state  = Connecting;

    ensureConnector();
    d->connector->connectToHost(services, transport, domain, port);
}

QAbstractSocket *BSocket::abstractSocket() const { return d->qsock; }

qintptr BSocket::socket() const
{
    if (d->qsock)
        return d->qsock->socketDescriptor();
    else
        return -1;
}

void BSocket::setSocket(QTcpSocket *s)
{
    resetConnection(true);
    s->setParent(this);
    d->qsock       = s;
    d->qsock_relay = new QTcpSocketSignalRelay(d->qsock, this);
    qs_connected_step2(false); // we have desriptor already. so it's already known to be connected
}

int BSocket::state() const { return d->state; }

const QString &BSocket::host() const { return d->host; }

bool BSocket::isOpen() const
{
    if (d->state == Connected)
        return true;
    else
        return false;
}

void BSocket::close()
{
    if (d->state == Idle)
        return;

    if (d->qsock) {
        d->state = Closing;
        d->qsock->close();
        if (d->qsock->state() == QAbstractSocket::ClosingState) {
            return; // wait for disconnected signal
        } else {
            resetConnection();
        }
    } else {
        resetConnection();
    }
}

qint64 BSocket::writeData(const char *data, qint64 maxSize)
{
    if (d->state != Connected)
        return 0;
    BSLOG(BSDEBUG << "- [" << maxSize << "]: {" << QByteArray::fromRawData(data, maxSize) << "}");
    return d->qsock->write(data, maxSize);
}

qint64 BSocket::readData(char *data, qint64 maxSize)
{
    if (!maxSize) {
        return 0;
    }
    qint64 readSize;
    if (d->qsock) {
        qint64 max = bytesAvailable();
        if (maxSize <= 0 || maxSize > max) {
            maxSize = max;
        }
        readSize = d->qsock->read(data, maxSize);
    } else {
        readSize = ByteStream::readData(data, maxSize);
    }

    BSLOG(BSDEBUG << "- [" << readSize << "]: {" << QByteArray::fromRawData(data, readSize) << "}");
    return readSize;
}

qint64 BSocket::bytesAvailable() const
{
    if (d->qsock)
        return d->qsock->bytesAvailable();
    else
        return ByteStream::bytesAvailable();
}

qint64 BSocket::bytesToWrite() const
{
    if (!d->qsock)
        return 0;
    return d->qsock->bytesToWrite();
}

QHostAddress BSocket::address() const
{
    if (d->qsock)
        return d->qsock->localAddress();
    else
        return QHostAddress();
}

quint16 BSocket::port() const
{
    if (d->qsock)
        return d->qsock->localPort();
    else
        return 0;
}

QHostAddress BSocket::peerAddress() const
{
    if (d->qsock)
        return d->qsock->peerAddress();
    else
        return QHostAddress();
}

quint16 BSocket::peerPort() const
{
    if (d->qsock)
        return d->qsock->peerPort();
    else
        return 0;
}

QString BSocket::service() const { return d->service; }

void BSocket::qs_connected()
{
    HappyEyeballsConnector::SockData sd = d->connector->takeCurrent(this);
    d->qsock                            = sd.sock;
    d->qsock_relay                      = sd.relay;
    d->host                             = sd.hostname;
    d->service                          = sd.service;
    d->connector->deleteLater();
    qs_connected_step2(true);
}

void BSocket::qs_connected_step2(bool signalConnected)
{
    connect(d->qsock_relay, SIGNAL(disconnected()), SLOT(qs_closed()));
    connect(d->qsock_relay, SIGNAL(readyRead()), SLOT(qs_readyRead()));
    connect(d->qsock_relay, SIGNAL(bytesWritten(qint64)), SLOT(qs_bytesWritten(qint64)));
    connect(d->qsock_relay, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(qs_error(QAbstractSocket::SocketError)));

    setOpenMode(QIODevice::ReadWrite);
    d->state = Connected;
    BSLOG(BSDEBUG << "Connected");
    QPointer<BSocket> valid(this);
    if (signalConnected) {
        emit connected();
    }

    if (valid && d->qsock->bytesAvailable()) {
        qs_readyRead();
    }
}

void BSocket::qs_closed()
{
    if (d->state == Closing) {
        BSLOG(BSDEBUG << "Delayed Close Finished");
        resetConnection();
        emit delayedCloseFinished();
    }
}

void BSocket::qs_readyRead() { emit readyRead(); }

void BSocket::qs_bytesWritten(qint64 x64)
{
    BSLOG(BSDEBUG << "BytesWritten [" << x64 << "]");
    emit bytesWritten(x64);
}

void BSocket::qs_error(QAbstractSocket::SocketError x)
{
    if (x == QTcpSocket::RemoteHostClosedError) {
        BSLOG(BSDEBUG << "Connection Closed");
        resetConnection();
        emit connectionClosed();
        return;
    }

    BSLOG(BSDEBUG << "Error");
    resetConnection();
    if (x == QTcpSocket::ConnectionRefusedError)
        emit error(ErrConnectionRefused);
    else if (x == QTcpSocket::HostNotFoundError)
        emit error(ErrHostNotFound);
    else
        emit error(ErrRead);
}

#include "bsocket.moc"

// CS_NAMESPACE_END
