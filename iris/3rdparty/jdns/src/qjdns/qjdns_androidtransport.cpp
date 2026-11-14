/*
 * Copyright (C) 2026  Psi contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "qjdns_querytransport.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)

#include <QHash>
#include <QHostInfo>
#include <QJniObject>
#include <QNetworkProxy>
#include <QSet>
#include <QSslSocket>
#include <QSocketNotifier>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QtCore/qcoreapplication.h>

#include <android/multinetwork.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr int SystemResolverTimeoutMs = 30000;
constexpr int HostLookupTimeoutMs = 10000;
constexpr int UdpFirstTimeoutMs = 1200;
constexpr int UdpRetryTimeoutMs = 2000;
constexpr int StreamTimeoutMs = 6000;
constexpr quint16 DnsPort = 53;
constexpr quint16 DotPort = 853;

class AndroidResolverApi
{
public:
    using ResNsend = int (*)(net_handle_t, const uint8_t *, size_t, uint32_t);
    using ResNresult = int (*)(int, int *, uint8_t *, size_t);
    using ResCancel = void (*)(int);
    using SetSockNetwork = int (*)(net_handle_t, int);

    AndroidResolverApi()
    {
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if(!library_)
            return;

        nsend = load<ResNsend>("android_res_nsend");
        nresult = load<ResNresult>("android_res_nresult");
        cancel = load<ResCancel>("android_res_cancel");
        setsocknetwork = load<SetSockNetwork>("android_setsocknetwork");
    }

    ~AndroidResolverApi()
    {
        if(library_)
            dlclose(library_);
    }

    bool resolverAvailable() const
    {
        return nsend && nresult && cancel;
    }

    bool socketBindingAvailable() const
    {
        return setsocknetwork != nullptr;
    }

    ResNsend nsend = nullptr;
    ResNresult nresult = nullptr;
    ResCancel cancel = nullptr;
    SetSockNetwork setsocknetwork = nullptr;

private:
    template<typename T>
    T load(const char *name) const
    {
        void *symbol = dlsym(library_, name);
        T function = nullptr;
        static_assert(sizeof(function) == sizeof(symbol), "function and data pointers must have the same size");
        std::memcpy(&function, &symbol, sizeof(function));
        return function;
    }

    void *library_ = nullptr;
};

AndroidResolverApi &resolverApi()
{
    static AndroidResolverApi api;
    return api;
}

QJniObject connectivityManager()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if(!context.isValid())
        return QJniObject();

    const QJniObject serviceName = QJniObject::fromString(QStringLiteral("connectivity"));
    return context.callObjectMethod(
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", serviceName.object<jstring>());
}

QJniObject activeNetwork()
{
    const QJniObject manager = connectivityManager();
    if(!manager.isValid())
        return QJniObject();

    return manager.callObjectMethod("getActiveNetwork", "()Landroid/net/Network;");
}

net_handle_t networkHandle(const QJniObject &network)
{
    if(!network.isValid())
        return 0;
    return static_cast<net_handle_t>(network.callMethod<jlong>("getNetworkHandle", "()J"));
}

struct AndroidNetworkInfo
{
    QJniObject network;
    net_handle_t handle = 0;
    QList<QHostAddress> dnsServers;
    bool privateDnsActive = false;
    QString privateDnsServerName;

    bool isValid() const
    {
        return network.isValid() && handle != 0;
    }
};

AndroidNetworkInfo currentNetworkInfo()
{
    AndroidNetworkInfo out;
    out.network = activeNetwork();
    out.handle = networkHandle(out.network);
    if(!out.isValid())
        return out;

    const QJniObject manager = connectivityManager();
    if(!manager.isValid())
        return out;

    const QJniObject linkProperties = manager.callObjectMethod(
        "getLinkProperties", "(Landroid/net/Network;)Landroid/net/LinkProperties;",
        out.network.object<jobject>());
    if(!linkProperties.isValid())
        return out;

    const QJniObject dnsServers = linkProperties.callObjectMethod(
        "getDnsServers", "()Ljava/util/List;");
    if(dnsServers.isValid())
    {
        const jint count = dnsServers.callMethod<jint>("size", "()I");
        for(jint i = 0; i < count; ++i)
        {
            const QJniObject inetAddress = dnsServers.callObjectMethod(
                "get", "(I)Ljava/lang/Object;", i);
            if(!inetAddress.isValid())
                continue;

            const QJniObject hostAddress = inetAddress.callObjectMethod(
                "getHostAddress", "()Ljava/lang/String;");
            if(!hostAddress.isValid())
                continue;

            QHostAddress address;
            if(address.setAddress(hostAddress.toString()))
                out.dnsServers += address;
        }
    }

    out.privateDnsActive = linkProperties.callMethod<jboolean>(
        "isPrivateDnsActive", "()Z");
    const QJniObject privateDnsServerName = linkProperties.callObjectMethod(
        "getPrivateDnsServerName", "()Ljava/lang/String;");
    if(privateDnsServerName.isValid())
        out.privateDnsServerName = privateDnsServerName.toString();

    return out;
}

quint16 dnsId(const QByteArray &packet)
{
    if(packet.size() < 2)
        return 0;
    return (static_cast<quint16>(static_cast<quint8>(packet[0])) << 8)
        | static_cast<quint8>(packet[1]);
}

bool isTruncatedDnsResponse(const QByteArray &packet)
{
    if(packet.size() < 4)
        return false;
    return (static_cast<quint8>(packet[2]) & 0x02) != 0;
}



QByteArray frameDnsMessage(const QByteArray &packet)
{
    if(packet.size() > 0xffff)
        return QByteArray();

    QByteArray framed;
    framed.reserve(packet.size() + 2);
    framed += static_cast<char>((packet.size() >> 8) & 0xff);
    framed += static_cast<char>(packet.size() & 0xff);
    framed += packet;
    return framed;
}

int createNetworkBoundStreamSocket(net_handle_t network, const QHostAddress &server)
{
    const int family = server.protocol() == QAbstractSocket::IPv6Protocol ? AF_INET6 : AF_INET;
    const int fd = ::socket(family, SOCK_STREAM, 0);
    if(fd < 0)
        return -1;

    AndroidResolverApi &api = resolverApi();
    if(!api.socketBindingAvailable() || api.setsocknetwork(network, fd) != 0)
    {
        ::close(fd);
        return -1;
    }

    return fd;
}

class QJDnsAndroidTransportBase : public QJDnsQueryTransport
{
public:
    explicit QJDnsAndroidTransportBase(QObject *parent)
        : QJDnsQueryTransport(parent)
    {
    }

    void cancelRequest(int id) override
    {
        if(!handles_.remove(id))
            return;
        cancelHandleRequests(id);
    }

protected:
    bool registerRequest(int id)
    {
        if(handles_.contains(id))
            return false;
        handles_.insert(id);
        return true;
    }

    virtual void cancelHandleRequests(int handle) = 0;

    void queueResponse(int handle, const QByteArray &data)
    {
        if(!handles_.contains(handle))
            return;
        QMetaObject::invokeMethod(this, [this, handle, data]() {
            // The consumer may cancel after the backend completed, before
            // this event is delivered. Never emit a stale DNS response.
            if(handles_.remove(handle))
                emit responseReady(handle, data);
        }, Qt::QueuedConnection);
    }

    void queueFailure(int handle, const QString &reason)
    {
        if(!handles_.contains(handle))
            return;
        const bool isTimeout = reason.contains(QStringLiteral("timed out"), Qt::CaseInsensitive);
        QMetaObject::invokeMethod(this, [this, handle, reason, isTimeout]() {
            if(handles_.remove(handle))
                emit failed(handle, reason, isTimeout ? Timeout : NetworkError);
        }, Qt::QueuedConnection);
    }

private:
    QSet<int> handles_;
};

class QJDnsAndroidResolverTransport : public QJDnsAndroidTransportBase
{
public:
    explicit QJDnsAndroidResolverTransport(QObject *parent)
        : QJDnsAndroidTransportBase(parent)
    {
    }

    ~QJDnsAndroidResolverTransport() override
    {
        const QList<int> fds = transactions_.keys();
        for(int fd : fds)
            cancelTransaction(fd);
    }

    void startRequest(int handle, const QByteArray &packet) override
    {
        if(!registerRequest(handle))
            return;
        if(packet.size() < 12)
        {
            queueFailure(handle, QStringLiteral("Invalid DNS request"));
            return;
        }


        const net_handle_t network = networkHandle(activeNetwork());
        if(network == 0)
        {
            queueFailure(handle, QStringLiteral("Android has no active network"));
            return;
        }

        AndroidResolverApi &api = resolverApi();
        const int fd = api.nsend(network,
                                 reinterpret_cast<const uint8_t *>(packet.constData()),
                                 static_cast<size_t>(packet.size()), 0);
        if(fd < 0)
        {
            queueFailure(handle,
                         QStringLiteral("android_res_nsend failed (%1)").arg(fd));
            return;
        }

        auto *transaction = new Transaction;
        transaction->handle = handle;
        transaction->notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        transaction->timer = new QTimer(this);
        transaction->timer->setSingleShot(true);
        transactions_.insert(fd, transaction);

        connect(transaction->notifier, &QSocketNotifier::activated, this,
                [this, fd](QSocketDescriptor, QSocketNotifier::Type) {
                    completeTransaction(fd);
                });
        connect(transaction->timer, &QTimer::timeout, this, [this, fd]() {
            timeoutTransaction(fd);
        });
        transaction->timer->start(SystemResolverTimeoutMs);

        return;
    }

protected:
    void cancelHandleRequests(int handle) override
    {
        const QList<int> fds = transactions_.keys();
        for(int fd : fds)
        {
            const Transaction *transaction = transactions_.value(fd);
            if(transaction && transaction->handle == handle)
                cancelTransaction(fd);
        }
    }

private:
    struct Transaction
    {
        int handle = 0;
        QSocketNotifier *notifier = nullptr;
        QTimer *timer = nullptr;
    };

    void disposeTransaction(Transaction *transaction)
    {
        if(transaction->notifier)
        {
            transaction->notifier->setEnabled(false);
            delete transaction->notifier;
        }
        delete transaction->timer;
        transaction->notifier = nullptr;
        transaction->timer = nullptr;
    }

    void completeTransaction(int fd)
    {
        Transaction *transaction = transactions_.take(fd);
        if(!transaction)
            return;

        disposeTransaction(transaction);

        QByteArray answer(65536, '\0');
        int rcode = 0;
        const int size = resolverApi().nresult(
            fd, &rcode, reinterpret_cast<uint8_t *>(answer.data()), static_cast<size_t>(answer.size()));

        if(size < 0)
        {
            queueFailure(transaction->handle,
                         QStringLiteral("android_res_nresult failed (%1)").arg(size));
        }
        else
        {
            answer.resize(size);
            queueResponse(transaction->handle, answer);
        }

        delete transaction;
    }

    void timeoutTransaction(int fd)
    {
        Transaction *transaction = transactions_.take(fd);
        if(!transaction)
            return;

        disposeTransaction(transaction);
        resolverApi().cancel(fd);
        queueFailure(transaction->handle,
                     QStringLiteral("Android system DNS resolver timed out"));
        delete transaction;
    }

    void cancelTransaction(int fd)
    {
        Transaction *transaction = transactions_.take(fd);
        if(!transaction)
            return;

        disposeTransaction(transaction);
        resolverApi().cancel(fd);
        delete transaction;
    }

    QHash<int, Transaction *> transactions_;
};

class QJDnsAndroidLegacyTransport : public QJDnsAndroidTransportBase
{
public:
    explicit QJDnsAndroidLegacyTransport(QObject *parent)
        : QJDnsAndroidTransportBase(parent)
    {
    }

    ~QJDnsAndroidLegacyTransport() override
    {
        const QList<quint64> keys = transactions_.keys();
        for(quint64 key : keys)
            cancelTransaction(key);
    }

    void startRequest(int handle, const QByteArray &packet) override
    {
        if(!registerRequest(handle))
            return;
        if(packet.size() < 12)
        {
            queueFailure(handle, QStringLiteral("Invalid DNS request"));
            return;
        }

        // The resolver ID is unique within the session; no DNS header ID,
        // synthetic socket handle, or second outstanding-key set is needed.
        const quint64 key = static_cast<quint64>(static_cast<quint32>(handle));

        auto *transaction = new Transaction;
        transaction->handle = handle;
        transaction->key = key;
        transaction->query = packet;
        transaction->network = currentNetworkInfo();
        transaction->timer = new QTimer(this);
        transaction->timer->setSingleShot(true);
        transactions_.insert(key, transaction);

        connect(transaction->timer, &QTimer::timeout, this, [this, key]() {
            transactionTimedOut(key);
        });

        if(!transaction->network.isValid())
        {
            finishFailure(transaction, QStringLiteral("Android has no active network"));
        }
        else if(!resolverApi().socketBindingAvailable())
        {
            finishFailure(transaction, QStringLiteral("android_setsocknetwork is unavailable"));
        }
        else if(transaction->network.privateDnsActive)
        {
            if(!QSslSocket::supportsSsl())
            {
                finishFailure(transaction, QStringLiteral("Private DNS is active but TLS support is unavailable"));
            }
            else if(!transaction->network.privateDnsServerName.isEmpty())
            {
                transaction->mode = Transaction::DotStrict;
                transaction->peerName = transaction->network.privateDnsServerName;
                startStrictServerLookup(transaction);
            }
            else if(transaction->network.dnsServers.isEmpty())
            {
                finishFailure(transaction, QStringLiteral("Private DNS is active but Android reported no DNS servers"));
            }
            else
            {
                transaction->mode = Transaction::DotOpportunistic;
                transaction->servers = transaction->network.dnsServers;
                transaction->maxRounds = 1;
                startNextServer(transaction);
            }
        }
        else if(transaction->network.dnsServers.isEmpty())
        {
            finishFailure(transaction, QStringLiteral("Android reported no DNS servers"));
        }
        else
        {
            transaction->mode = Transaction::Udp;
            transaction->servers = transaction->network.dnsServers;
            transaction->maxRounds = 2;
            startNextServer(transaction);
        }

        return;
    }

protected:
    void cancelHandleRequests(int handle) override
    {
        const QList<quint64> keys = transactions_.keys();
        for(quint64 key : keys)
        {
            const Transaction *transaction = transactions_.value(key);
            if(transaction && transaction->handle == handle)
                cancelTransaction(key);
        }
    }

private:
    struct Transaction
    {
        enum Mode {
            Udp,
            DotOpportunistic,
            DotStrict
        };

        int handle = 0;
        quint64 key = 0;
        QByteArray query;
        AndroidNetworkInfo network;
        Mode mode = Udp;
        QString peerName;
        QList<QHostAddress> servers;
        int serverIndex = 0;
        int round = 0;
        int maxRounds = 1;
        QHostAddress currentServer;
        int lookupId = -1;
        QTimer *timer = nullptr;
        QUdpSocket *udp = nullptr;
        QAbstractSocket *stream = nullptr;
        QByteArray streamBuffer;
        int expectedStreamSize = -1;
    };

    void startStrictServerLookup(Transaction *transaction)
    {
        transaction->timer->start(HostLookupTimeoutMs);
        const quint64 key = transaction->key;
        transaction->lookupId = QHostInfo::lookupHost(
            transaction->peerName, this, [this, key](const QHostInfo &info) {
                Transaction *transaction = transactions_.value(key);
                if(!transaction)
                    return;

                transaction->lookupId = -1;
                transaction->timer->stop();
                if(info.error() != QHostInfo::NoError || info.addresses().isEmpty())
                {
                    finishFailure(transaction,
                                  QStringLiteral("Unable to resolve strict Private DNS server %1: %2")
                                      .arg(transaction->peerName, info.errorString()));
                    return;
                }

                transaction->servers = info.addresses();
                transaction->maxRounds = 1;
                startNextServer(transaction);
            });
    }

    void startNextServer(Transaction *transaction)
    {
        cleanupAttempt(transaction);

        if(transaction->servers.isEmpty())
        {
            finishFailure(transaction, QStringLiteral("No DNS server addresses are available"));
            return;
        }

        if(transaction->serverIndex >= transaction->servers.size())
        {
            ++transaction->round;
            if(transaction->round >= transaction->maxRounds)
            {
                finishFailure(transaction, QStringLiteral("All Android DNS servers failed"));
                return;
            }
            transaction->serverIndex = 0;
        }

        transaction->currentServer = transaction->servers.at(transaction->serverIndex++);
        if(transaction->mode == Transaction::Udp)
            startUdp(transaction);
        else
            startDot(transaction);
    }

    void startUdp(Transaction *transaction)
    {
        auto *socket = new QUdpSocket(this);
        transaction->udp = socket;

        const QHostAddress bindAddress = transaction->currentServer.protocol() == QAbstractSocket::IPv6Protocol
            ? QHostAddress(QHostAddress::AnyIPv6)
            : QHostAddress(QHostAddress::AnyIPv4);
        if(!socket->bind(bindAddress, 0))
        {
            emit debugLine(QStringLiteral("Unable to bind Android DNS UDP socket: %1").arg(socket->errorString()));
            startNextServer(transaction);
            return;
        }

        const int fd = static_cast<int>(socket->socketDescriptor());
        if(fd < 0 || resolverApi().setsocknetwork(transaction->network.handle, fd) != 0)
        {
            emit debugLine(QStringLiteral("Unable to bind DNS UDP socket to Android Network"));
            startNextServer(transaction);
            return;
        }

        const quint64 key = transaction->key;
        connect(socket, &QUdpSocket::readyRead, this, [this, key, socket]() {
            Transaction *transaction = transactions_.value(key);
            if(!transaction || transaction->udp != socket)
                return;
            readUdp(transaction);
        });
        connect(socket, &QUdpSocket::errorOccurred, this,
                [this, key, socket](QAbstractSocket::SocketError) {
                    Transaction *transaction = transactions_.value(key);
                    if(!transaction || transaction->udp != socket)
                        return;
                    emit debugLine(QStringLiteral("Android DNS UDP error: %1").arg(socket->errorString()));
                    startNextServer(transaction);
                });

        if(socket->writeDatagram(transaction->query, transaction->currentServer, DnsPort) < 0)
        {
            emit debugLine(QStringLiteral("Unable to send Android DNS UDP query: %1").arg(socket->errorString()));
            startNextServer(transaction);
            return;
        }

        transaction->timer->start(transaction->round == 0 ? UdpFirstTimeoutMs : UdpRetryTimeoutMs);
    }

    void readUdp(Transaction *transaction)
    {
        while(transaction->udp && transaction->udp->hasPendingDatagrams())
        {
            const qint64 size = transaction->udp->pendingDatagramSize();
            if(size < 0)
                return;

            QByteArray answer(static_cast<int>(size), '\0');
            QHostAddress source;
            quint16 sourcePort = 0;
            const qint64 read = transaction->udp->readDatagram(
                answer.data(), answer.size(), &source, &sourcePort);
            if(read < 0)
                return;
            answer.resize(static_cast<int>(read));

            if(source != transaction->currentServer || sourcePort != DnsPort)
                continue;
            if(answer.size() < 12 || dnsId(answer) != dnsId(transaction->query))
                continue;

            if(isTruncatedDnsResponse(answer))
            {
                const QHostAddress server = transaction->currentServer;
                cleanupAttempt(transaction);
                startPlainTcp(transaction, server);
                return;
            }

            finishResponse(transaction, answer);
            return;
        }
    }

    bool prepareStreamSocket(Transaction *transaction, QAbstractSocket *socket, const QHostAddress &server)
    {
        socket->setProxy(QNetworkProxy::NoProxy);
        const int fd = createNetworkBoundStreamSocket(transaction->network.handle, server);
        if(fd < 0)
            return false;
        if(!socket->setSocketDescriptor(fd, QAbstractSocket::UnconnectedState, QIODevice::ReadWrite))
        {
            ::close(fd);
            return false;
        }
        return true;
    }

    void startPlainTcp(Transaction *transaction, const QHostAddress &server)
    {
        auto *socket = new QTcpSocket(this);
        transaction->stream = socket;
        transaction->currentServer = server;
        transaction->streamBuffer.clear();
        transaction->expectedStreamSize = -1;

        if(!prepareStreamSocket(transaction, socket, server))
        {
            emit debugLine(QStringLiteral("Unable to create Android DNS TCP socket"));
            startNextServer(transaction);
            return;
        }

        const quint64 key = transaction->key;
        connect(socket, &QTcpSocket::connected, this, [this, key, socket]() {
            Transaction *transaction = transactions_.value(key);
            if(!transaction || transaction->stream != socket)
                return;
            writeStreamQuery(transaction, socket);
        });
        connect(socket, &QTcpSocket::readyRead, this, [this, key, socket]() {
            Transaction *transaction = transactions_.value(key);
            if(!transaction || transaction->stream != socket)
                return;
            readStream(transaction, socket);
        });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, key, socket](QAbstractSocket::SocketError) {
                    Transaction *transaction = transactions_.value(key);
                    if(!transaction || transaction->stream != socket)
                        return;
                    emit debugLine(QStringLiteral("Android DNS TCP error: %1").arg(socket->errorString()));
                    startNextServer(transaction);
                });

        socket->connectToHost(server, DnsPort);
        transaction->timer->start(StreamTimeoutMs);
    }

    void startDot(Transaction *transaction)
    {
        auto *socket = new QSslSocket(this);
        transaction->stream = socket;
        transaction->streamBuffer.clear();
        transaction->expectedStreamSize = -1;

        if(!prepareStreamSocket(transaction, socket, transaction->currentServer))
        {
            emit debugLine(QStringLiteral("Unable to create Android Private DNS TLS socket"));
            startNextServer(transaction);
            return;
        }

        const bool strict = transaction->mode == Transaction::DotStrict;
        socket->setPeerVerifyMode(strict ? QSslSocket::VerifyPeer : QSslSocket::VerifyNone);

        const quint64 key = transaction->key;
        connect(socket, &QSslSocket::encrypted, this, [this, key, socket]() {
            Transaction *transaction = transactions_.value(key);
            if(!transaction || transaction->stream != socket)
                return;
            writeStreamQuery(transaction, socket);
        });
        connect(socket, &QSslSocket::readyRead, this, [this, key, socket]() {
            Transaction *transaction = transactions_.value(key);
            if(!transaction || transaction->stream != socket)
                return;
            readStream(transaction, socket);
        });
        connect(socket, &QSslSocket::sslErrors, this,
                [this, key, socket](const QList<QSslError> &errors) {
                    Transaction *transaction = transactions_.value(key);
                    if(!transaction || transaction->stream != socket)
                        return;
                    if(transaction->mode == Transaction::DotStrict && !errors.isEmpty())
                        emit debugLine(QStringLiteral("Strict Android Private DNS TLS verification failed: %1")
                                           .arg(errors.first().errorString()));
                });
        connect(socket, &QSslSocket::errorOccurred, this,
                [this, key, socket](QAbstractSocket::SocketError) {
                    Transaction *transaction = transactions_.value(key);
                    if(!transaction || transaction->stream != socket)
                        return;
                    emit debugLine(QStringLiteral("Android Private DNS TLS error: %1").arg(socket->errorString()));
                    startNextServer(transaction);
                });

        const QString peerName = strict ? transaction->peerName : QString();
        socket->connectToHostEncrypted(transaction->currentServer.toString(), DotPort, peerName);
        transaction->timer->start(StreamTimeoutMs);
    }

    void writeStreamQuery(Transaction *transaction, QIODevice *socket)
    {
        const QByteArray framed = frameDnsMessage(transaction->query);
        if(framed.isEmpty() || socket->write(framed) != framed.size())
        {
            emit debugLine(QStringLiteral("Unable to write framed Android DNS query"));
            startNextServer(transaction);
        }
    }

    void readStream(Transaction *transaction, QIODevice *socket)
    {
        transaction->streamBuffer += socket->readAll();
        if(transaction->expectedStreamSize < 0 && transaction->streamBuffer.size() >= 2)
        {
            transaction->expectedStreamSize =
                (static_cast<int>(static_cast<quint8>(transaction->streamBuffer[0])) << 8)
                | static_cast<int>(static_cast<quint8>(transaction->streamBuffer[1]));
            transaction->streamBuffer.remove(0, 2);
            if(transaction->expectedStreamSize <= 0)
            {
                startNextServer(transaction);
                return;
            }
        }

        if(transaction->expectedStreamSize >= 0
            && transaction->streamBuffer.size() >= transaction->expectedStreamSize)
        {
            QByteArray answer = transaction->streamBuffer.left(transaction->expectedStreamSize);
            if(answer.size() < 12 || dnsId(answer) != dnsId(transaction->query))
            {
                startNextServer(transaction);
                return;
            }
            finishResponse(transaction, answer);
        }
    }

    void transactionTimedOut(quint64 key)
    {
        Transaction *transaction = transactions_.value(key);
        if(!transaction)
            return;

        if(transaction->lookupId != -1)
        {
            QHostInfo::abortHostLookup(transaction->lookupId);
            transaction->lookupId = -1;
            finishFailure(transaction, QStringLiteral("Strict Android Private DNS hostname lookup timed out"));
            return;
        }

        emit debugLine(QStringLiteral("Android DNS server %1 timed out")
                           .arg(transaction->currentServer.toString()));
        startNextServer(transaction);
    }

    void cleanupAttempt(Transaction *transaction)
    {
        transaction->timer->stop();
        if(transaction->udp)
        {
            transaction->udp->disconnect(this);
            transaction->udp->deleteLater();
            transaction->udp = nullptr;
        }
        if(transaction->stream)
        {
            transaction->stream->disconnect(this);
            transaction->stream->abort();
            transaction->stream->deleteLater();
            transaction->stream = nullptr;
        }
        transaction->streamBuffer.clear();
        transaction->expectedStreamSize = -1;
    }

    void finishResponse(Transaction *transaction, const QByteArray &answer)
    {
        const int handle = transaction->handle;
        const quint64 key = transaction->key;
        cleanupAttempt(transaction);
        transactions_.remove(key);
        delete transaction->timer;
        transaction->timer = nullptr;
        delete transaction;
        queueResponse(handle, answer);
    }

    void finishFailure(Transaction *transaction, const QString &reason)
    {
        const int handle = transaction->handle;
        const quint64 key = transaction->key;
        if(transaction->lookupId != -1)
            QHostInfo::abortHostLookup(transaction->lookupId);
        cleanupAttempt(transaction);
        transactions_.remove(key);
        delete transaction->timer;
        transaction->timer = nullptr;
        delete transaction;
        queueFailure(handle, reason);
    }

    void cancelTransaction(quint64 key)
    {
        Transaction *transaction = transactions_.take(key);
        if(!transaction)
            return;

        if(transaction->lookupId != -1)
            QHostInfo::abortHostLookup(transaction->lookupId);
        cleanupAttempt(transaction);
        delete transaction->timer;
        transaction->timer = nullptr;
        delete transaction;
    }

    QHash<quint64, Transaction *> transactions_;
};

} // namespace

QJDnsQueryTransport *qjdns_create_android_query_transport(QObject *parent)
{
    const int sdk = QNativeInterface::QAndroidApplication::sdkVersion();
    if(sdk >= 29 && resolverApi().resolverAvailable())
        return new QJDnsAndroidResolverTransport(parent);
    if(sdk >= 28)
        return new QJDnsAndroidLegacyTransport(parent);
    return nullptr;
}

#else

QJDnsQueryTransport *qjdns_create_android_query_transport(QObject *)
{
    return nullptr;
}

#endif
