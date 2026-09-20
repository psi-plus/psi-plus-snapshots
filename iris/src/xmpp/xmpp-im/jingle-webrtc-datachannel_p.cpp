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
#include <QTimer>

#include <cstring>
#include <limits>

namespace XMPP { namespace Jingle { namespace SCTP {

    WebRTCDataChannel::WebRTCDataChannel(AssociationPrivate *association, quint8 channelType, quint32 reliability,
                                         quint16 priority, const QString &label, const QString &protocol,
                                         DcepState state) :
        association(association), channelType(channelType), reliability(reliability), priority(priority), label(label),
        protocol(protocol), dcepState(state)
    {
    }

    QSharedPointer<WebRTCDataChannel> WebRTCDataChannel::fromChannelOpen(AssociationPrivate *assoc,
                                                                         const QByteArray   &data)
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
        quint16 protocolLength = qFromBigEndian<quint16>(data.data() + 10);
        const qsizetype labelOffset    = 12;
        const qsizetype protocolOffset = labelOffset + qsizetype(labelLength);
        if (protocolOffset > data.size() || qsizetype(protocolLength) > data.size() - protocolOffset) {
            qWarning("jingle-sctp: truncated label or protocol in header for WebRTC DataChannel DATA_CHANNEL_OPEN. "
                     "Dropping..");
            return {};
        }
        QString label    = QString::fromUtf8(data.data() + labelOffset, labelLength);
        QString protocol = QString::fromUtf8(data.data() + protocolOffset, protocolLength);
        // start with DcepNegotiated since caller will ack asap
        auto channel       = QSharedPointer<WebRTCDataChannel>::create(assoc, channelType, reliability, priority, label,
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

        if (utf8Label.size() > std::numeric_limits<quint16>::max()
            || utf8Protocol.size() > std::numeric_limits<quint16>::max()) {
            qWarning("jingle-sctp: WebRTC DataChannel label or protocol is too long");
            onError(QAbstractSocket::SocketResourceError);
            return;
        }

        const qsizetype protocolOffset = 12 + utf8Label.size();
        QByteArray      data(protocolOffset + utf8Protocol.size(), 0);

        data[0] = DCEP_DATA_CHANNEL_OPEN;
        data[1] = channelType;
        qToBigEndian(priority, data.data() + 2);
        qToBigEndian(reliability, data.data() + 4);
        qToBigEndian<quint16>(quint16(utf8Label.size()), data.data() + 8);
        qToBigEndian<quint16>(quint16(utf8Protocol.size()), data.data() + 10);
        data.replace(12, utf8Label.size(), utf8Label);
        data.replace(protocolOffset, utf8Protocol.size(), utf8Protocol);

        dcepState = DcepOpening;
        association->write(data, streamId, PPID_DCEP);
    }

    void WebRTCDataChannel::setOutgoingCallback(OutgoingCallback &&callback) { outgoingCallback = std::move(callback); }

    bool WebRTCDataChannel::hasPendingDatagrams() const { return datagrams.size() > 0; }

    QNetworkDatagram WebRTCDataChannel::readDatagram(qint64 maxSize)
    {
        Q_UNUSED(maxSize) // TODO or not?
        if (datagrams.size()) {
            auto dg = datagrams.takeFirst();
            _bytesAvailable -= dg.data().size();
            // Do not emit connectionClosed synchronously from inside readDatagram():
            // the caller has not yet accounted the returned bytes. Defer the drain
            // edge to the event loop so application state observes the payload first.
            QTimer::singleShot(0, this, [this]() { finishCloseIfDrained(); });
            return dg;
        }
        finishCloseIfDrained();
        return {};
    }

    bool WebRTCDataChannel::writeDatagram(const QNetworkDatagram &data)
    {
        Q_ASSERT(bool(outgoingCallback));
        outgoingBufSize += data.data().size();
        outgoingCallback({ quint16(streamId), channelType, PPID_BINARY, reliability, data.data() });
        return true;
    }

    qint64 WebRTCDataChannel::bytesAvailable() const
    {
        return tail.size() + _bytesAvailable + Connection::bytesAvailable();
    }

    qint64 WebRTCDataChannel::bytesToWrite() const { return outgoingBufSize + Connection::bytesToWrite(); }

    qint64 WebRTCDataChannel::readDataInternal(char *buf, qint64 sz)
    {
        qint64 actualSz = 0;
        do {
            if (tail.isEmpty() && !datagrams.isEmpty()) {
                tail = datagrams.takeFirst().data();
            }
            auto dataSz = std::min(sz, qint64(tail.size()));
            std::memcpy(buf + actualSz, tail.data(), dataSz);
            if (dataSz == qint64(tail.size())) {
                tail.clear();
            } else {
                tail.remove(0, dataSz);
                if (!tail.isEmpty()) {
                    break;
                }
            }
            actualSz += dataSz;
            sz -= dataSz;
        } while (sz > 0 && !datagrams.isEmpty());
        _bytesAvailable -= actualSz;
        // read() has the same ordering requirement as readDatagram(): closure
        // notification must not overtake accounting of the bytes being returned.
        QTimer::singleShot(0, this, [this]() { finishCloseIfDrained(); });
        // qDebug("read %lld bytes. more %lld is available", actualSz, _bytesAvailable);
        return actualSz;
    }

    void WebRTCDataChannel::close()
    {
        if (closeRequested || closeSignalEmitted)
            return;
        closeRequested = true;

        if (streamClosed || streamId < 0 || !association) {
            XMPP::Jingle::Connection::close();
            closeWasLocal = true;
            streamClosed  = true;
            finishCloseIfDrained();
            return;
        }

        // Stop accepting application writes immediately, but keep the read
        // side alive until SCTP confirms the stream reset. Buffered peer data
        // must remain readable during that finishing window.
        if (openMode() & QIODevice::WriteOnly)
            setOpenMode(openMode() & ~QIODevice::WriteOnly);
        association->close(quint16(streamId));
    }

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
        if (streamClosed)
            return;

        closeWasLocal     = closeRequested;
        streamClosed      = true;
        streamId          = -1;
        disconnectReason  = reason;
        if (openMode() & QIODevice::WriteOnly)
            setOpenMode(openMode() & ~QIODevice::WriteOnly);
        emit disconnected();
        finishCloseIfDrained();
    }

    void WebRTCDataChannel::finishCloseIfDrained()
    {
        if (!streamClosed || closeSignalEmitted || bytesAvailable() > 0)
            return;

        closeSignalEmitted = true;
        setOpenMode(QIODevice::NotOpen);
        if (closeWasLocal)
            emit delayedCloseFinished();
        else
            emit connectionClosed();
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
        datagrams.append(QNetworkDatagram { data });
        _bytesAvailable += data.size();
        // qDebug("datachannel readyread");
        emit readyRead();
    }

    void WebRTCDataChannel::onMessageWritten(size_t size)
    {
        outgoingBufSize -= size;
        emit bytesWritten(size);
    }
}}}
