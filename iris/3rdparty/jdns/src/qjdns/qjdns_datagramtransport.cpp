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

#include "qjdns_datagramtransport_p.h"

#include "qjdns_sock.h"

#include <QHash>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QUdpSocket>
#include <QVariant>

class QJDnsUdpTransport : public QJDnsDatagramTransport
{
public:
    explicit QJDnsUdpTransport(QObject *parent, const QNetworkInterface &interface)
        : QJDnsDatagramTransport(parent), interface_(interface) {}

    int open(const QHostAddress &address, quint16 port, const QHostAddress &multicastAddress) override
    {
        auto *socket = new QUdpSocket(this);
        if(!multicastAddress.isNull())
            socket->setProxy(QNetworkProxy::NoProxy); // link-local traffic stays on its selected link

        QUdpSocket::BindMode bindMode;
        bindMode |= QUdpSocket::ShareAddress;
        bindMode |= QUdpSocket::ReuseAddressHint;
        if(!socket->bind(address, port, bindMode))
        {
            delete socket;
            return 0;
        }

        if(!multicastAddress.isNull())
        {
            if(interface_.isValid())
            {
                socket->setMulticastInterface(interface_);
                if(socket->multicastInterface().index() != interface_.index())
                {
                    emit debugLine(QStringLiteral("Cannot select the requested multicast interface"));
                    delete socket;
                    return 0;
                }
                socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
                socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
                if(!socket->joinMulticastGroup(multicastAddress, interface_))
                {
                    emit debugLine(socket->errorString());
                    delete socket;
                    return 0;
                }
            }
            else
            {
                const int sd = socket->socketDescriptor();
                bool ok;
                int errorCode;
                if(multicastAddress.protocol() == QAbstractSocket::IPv6Protocol)
                    ok = qjdns_sock_setMulticast6(sd, multicastAddress.toIPv6Address().c, &errorCode);
                else
                    ok = qjdns_sock_setMulticast4(sd, multicastAddress.toIPv4Address(), &errorCode);

                if(!ok)
                {
                    emit debugLine(QString("failed to setup multicast on the socket (errorCode=%1)").arg(errorCode));
                    delete socket;
                    return 0;
                }

                if(multicastAddress.protocol() == QAbstractSocket::IPv6Protocol)
                {
                    qjdns_sock_setTTL6(sd, 255);
                    qjdns_sock_setIPv6Only(sd);
                }
                else
                    qjdns_sock_setTTL4(sd, 255);
            }
        }

        const int handle = nextHandle_++;
        sockets_.insert(handle, SocketState(socket));

        connect(socket, &QUdpSocket::readyRead, this, [this, handle]() {
            auto state = sockets_.find(handle);
            if(state == sockets_.end())
                return;
            state->readNotified = true;
            emit readyRead(handle);
        });
        connect(socket, &QUdpSocket::bytesWritten, this, [this](qint64) { emit packetWritten(); }, Qt::QueuedConnection);
        return handle;
    }

    void close(int handle) override
    {
        auto *socket = sockets_.take(handle).socket;
        delete socket;
    }

    SubmitResult submit(int handle, const QHostAddress &destination, quint16 port, const QByteArray &packet) override
    {
        auto *socket = sockets_.value(handle).socket;
        if(!socket)
            return RetryLater;

        QNetworkDatagram datagram(packet, destination, port);
        bool perPacketInterface = interface_.isValid();
#ifdef Q_OS_WIN
        // The socket's multicast interface already pins these packets to the selected link.
        // Adding per-packet IP_PKTINFO can silently drop IPv4 multicast on Windows.
        // Retain per-packet selection for unicast replies.
        if(destination.protocol() == QAbstractSocket::IPv4Protocol && destination.isMulticast())
            perPacketInterface = false;
#endif
        if(perPacketInterface)
            datagram.setInterfaceIndex(uint(interface_.index()));
        if(socket->writeDatagram(datagram) == -1)
        {
            // Preserve the historical QJDns behaviour: a datagram that cannot
            // be written is treated as dropped rather than retried forever.
            return Dropped;
        }
        return Submitted;
    }

    bool takeResponse(int handle, Packet *packet) override
    {
        auto state = sockets_.find(handle);
        if(state == sockets_.end())
            return false;
        auto *socket = state->socket;
        const bool notified = state->readNotified;
        state->readNotified = false;
        if(!notified && !socket->hasPendingDatagrams())
            return false;

        do
        {
            // A stale readyRead notification still needs a read to rearm Qt's UDP notifier.
            // An explicit bound also avoids receiveDatagram() returning before that read when empty.
            constexpr qint64 MaxDatagramSize = 65535;
            const qint64 pendingSize = socket->pendingDatagramSize();
            const qint64 receiveSize = pendingSize < 0 ? MaxDatagramSize : qMin(pendingSize, MaxDatagramSize);
            const QNetworkDatagram datagram = socket->receiveDatagram(receiveSize);
            if(!datagram.isValid())
                continue;
            // A wildcard-bound socket can see packets received on another
            // interface when other processes join the same multicast group.
            if(interface_.isValid() && datagram.interfaceIndex() != 0
               && datagram.interfaceIndex() != uint(interface_.index()))
                continue;
            packet->sourceAddress = datagram.senderAddress();
            packet->sourcePort = datagram.senderPort();
            packet->data = datagram.data();
            return true;
        } while(socket->hasPendingDatagrams());
        return false;
    }

private:
    struct SocketState
    {
        explicit SocketState(QUdpSocket *s = nullptr) : socket(s), readNotified(false) {}
        QUdpSocket *socket;
        bool readNotified;
    };

    int nextHandle_ = 1;
    QHash<int, SocketState> sockets_;
    QNetworkInterface interface_;
};

QJDnsDatagramTransport *qjdns_create_datagram_transport(QObject *parent, const QNetworkInterface &interface)
{
    return new QJDnsUdpTransport(parent, interface);
}
