/*
 * jignle-webrtc-datachannel_p.h - WebRTC DataChannel implementation
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

#include "jingle-connection.h"

namespace XMPP { namespace Jingle { namespace SCTP {

    enum : quint32 {
        PPID_DCEP         = 50,
        PPID_STRING       = 51,
        PPID_BINARY       = 53,
        PPID_STRING_EMPTY = 56,
        PPID_BINARY_EMPTY = 57
    };
    enum : quint8 { DCEP_DATA_CHANNEL_ACK = 0x02, DCEP_DATA_CHANNEL_OPEN = 0x03 };

    class AssociationPrivate;
    class WebRTCDataChannel : public XMPP::Jingle::Connection {
        Q_OBJECT
    public:
        enum DisconnectReason { TransportClosed, SctpClosed, ChannelClosed, ChannelReplaced };
        enum DcepState { NoDcep, DcepOpening, DcepNegotiated };

        AssociationPrivate *   association;
        QList<NetworkDatagram> datagrams;
        DisconnectReason       disconnectReason = ChannelClosed;

        quint8    channelType = 0;
        quint32   reliability = 0;
        quint16   priority    = 256;
        QString   label;
        QString   protocol;
        int       streamId  = -1;
        DcepState dcepState = NoDcep;

        WebRTCDataChannel(AssociationPrivate *association, quint8 channelType = 0, quint32 reliability = 0,
                          quint16 priority = 256, const QString &label = QString(), const QString &protocol = QString(),
                          DcepState state = NoDcep);
        static QSharedPointer<WebRTCDataChannel> fromChannelOpen(AssociationPrivate *assoc, const QByteArray &data);

        void setStreamId(quint16 id) { streamId = id; }
        void connect();

        bool              hasPendingDatagrams() const override;
        NetworkDatagram   receiveDatagram(qint64 maxSize = -1) override;
        bool              sendDatagram(const NetworkDatagram &data) override;
        qint64            bytesAvailable() const override;
        qint64            bytesToWrite() const override;
        void              close() override;
        TransportFeatures features() const override;

    public:
        void onConnected();
        void onError(QAbstractSocket::SocketError error);
        void onDisconnected(DisconnectReason reason);
        void onIncomingData(const QByteArray &data, quint32 ppid);
    };

}}}
