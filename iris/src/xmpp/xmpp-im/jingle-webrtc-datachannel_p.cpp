/*
 * jignle-webrtc-datachannel_p.cpp - WebRTC DataChannel implementation
 * Copyright (C) 2021  Sergey Ilinykh
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

#include "jingle-webrtc-datachannel_p.h"
#include "jingle-sctp-association_p.h"

#include <QtEndian>

namespace XMPP { namespace Jingle { namespace SCTP {

    WebRTCDataChannel::WebRTCDataChannel(AssociationPrivate *association, quint8 channelType, quint32 reliability,
                                         quint16 priority, const QString &label, const QString &protocol,
                                         DcepState state) :
        association(association),
        channelType(channelType), reliability(reliability), priority(priority), label(label), protocol(protocol),
        dcepState(state)
    {
    }

    QSharedPointer<WebRTCDataChannel> WebRTCDataChannel::fromChannelOpen(AssociationPrivate *assoc,
                                                                         const QByteArray &  data)
    {
        /*
          0                   1                   2                   3
          0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         |  Message Type |  Channel Type |            Priority           |
         +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         |                    Reliability Parameter                      |
         +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         |         Label Length          |       Protocol Length         |
         +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         \                                                               /
         |                             Label                             |
         /                                                               \
         +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         \                                                               /
         |                            Protocol                           |
         /                                                               \
         +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        */
        if (data.size() < 12) {
            qWarning("jingle-sctp: truncated header for WebRTC DataChannel DATA_CHANNEL_OPEN. Dropping..");
            return {};
        }

        quint8  channelType    = data[1];
        quint16 priority       = qFromBigEndian<quint16>(data.data() + 2);
        quint32 reliability    = qFromBigEndian<quint32>(data.data() + 4);
        quint16 labelLength    = qFromBigEndian<quint16>(data.data() + 8);
        auto    protoOff       = (12 + labelLength) + labelLength % 4;
        quint16 protocolLength = qFromBigEndian<quint16>(data.data() + 10);
        if (protoOff + protocolLength > data.size()) {
            qWarning("jingle-sctp: truncated label or protocol in header for WebRTC DataChannel DATA_CHANNEL_OPEN. "
                     "Dropping..");
            return {};
        }
        QString label    = QString::fromUtf8(data.data() + 12, labelLength);
        QString protocol = QString::fromUtf8(data.data() + protoOff, protocolLength);
        // start with DcepNegotiated since caller will ack asap
        auto channel       = QSharedPointer<WebRTCDataChannel>::create(assoc, channelType, priority, reliability, label,
                                                                 protocol, DcepNegotiated);
        channel->_isRemote = true;
        channel->setOpenMode(QIODevice::ReadWrite);
        return channel;
    }

    void WebRTCDataChannel::connect()
    {
        // Q_ASSERT(streamId == -1);
        auto utf8Label    = label.toUtf8();
        auto utf8Protocol = protocol.toUtf8();

        auto sz       = 12 + utf8Label.size();
        auto protoOff = (sz + 3) & ~3;
        sz += (utf8Protocol.size() + 3) & ~3;
        QByteArray data(sz, 0);

        data[0] = DCEP_DATA_CHANNEL_OPEN;
        data[1] = channelType;
        qToBigEndian(priority, data.data() + 2);
        qToBigEndian(reliability, data.data() + 4);
        qToBigEndian(utf8Label.size(), data.data() + 8);
        qToBigEndian(utf8Protocol.size(), data.data() + 10);
        data.replace(12, utf8Label.size(), utf8Label);
        data.replace(protoOff, utf8Protocol.size(), utf8Protocol);

        dcepState = DcepOpening;
        association->write(data, streamId, PPID_DCEP);
    }

    void WebRTCDataChannel::setOutgoingCallback(OutgoingCallback &&callback) { outgoingCallback = std::move(callback); }

    bool WebRTCDataChannel::hasPendingDatagrams() const { return datagrams.size() > 0; }

    NetworkDatagram WebRTCDataChannel::readDatagram(qint64 maxSize)
    {
        Q_UNUSED(maxSize) // TODO or not?
        return datagrams.size() ? datagrams.takeFirst() : NetworkDatagram();
    }

    bool WebRTCDataChannel::writeDatagram(const NetworkDatagram &data)
    {
        Q_ASSERT(bool(outgoingCallback));
        outgoingBufSize += data.data().size();
        outgoingCallback({ quint16(streamId), channelType, PPID_BINARY, reliability, data.data() });
        return true;
    }

    qint64 WebRTCDataChannel::bytesAvailable() const { return 0; }

    qint64 WebRTCDataChannel::bytesToWrite() const { return 0; /*client->bytesToWrite();*/ }

    void WebRTCDataChannel::close() { XMPP::Jingle::Connection::close(); }

    TransportFeatures WebRTCDataChannel::features() const
    {
        // FIXME return proper featuers
        return TransportFeature::DataOriented | TransportFeature::Reliable | TransportFeature::Ordered
            | TransportFeature::Fast | TransportFeature::MessageOriented;
    }

    void WebRTCDataChannel::onConnected()
    {
        qDebug("jingle-sctp: channel connected!");
        emit connected();
    }

    void WebRTCDataChannel::onError(QAbstractSocket::SocketError error)
    {
        qDebug("jingle-ice: channel failed: %d", error);
    }

    void WebRTCDataChannel::onDisconnected(DisconnectReason reason)
    {
        if (!(openMode() & QIODevice::WriteOnly))
            return;
        streamId         = -1;
        disconnectReason = reason;
        setOpenMode(openMode() & ~QIODevice::WriteOnly);
        emit disconnected();
    }

    void WebRTCDataChannel::onIncomingData(const QByteArray &data, quint32 ppid)
    {
        if (ppid == PPID_DCEP) {
            if (dcepState == NoDcep) {
                qWarning("jingle-sctp: got dcep on prenegotiated datachannel");
                return;
            }
            if (data.isEmpty() || data[0] != DCEP_DATA_CHANNEL_ACK || dcepState != DcepOpening) {
                qWarning("jingle-sctp: unexpected DCEP. ignoring");
                return;
            }
            setOpenMode(QIODevice::ReadWrite);
            emit connected();
            return;
        }
        // check other PPIDs.
        datagrams.append(NetworkDatagram { data });
        emit readyRead();
    }

    void WebRTCDataChannel::onMessageWritten(size_t size)
    {
        outgoingBufSize -= size;
        emit bytesWritten(size);
    }
}}}
