/*
 * jignle-sctp.cpp - Jingle SCTP
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

#ifndef JINGLE_SCTP_H
#define JINGLE_SCTP_H

#include "jingle-connection.h"

#include <QAbstractSocket>
#include <QDomElement>
#include <QObject>

#include <memory>

class QDomDocument;

namespace XMPP { namespace Jingle { namespace SCTP {
    enum class Protocol { None, WebRTCDataChannel };
    enum Reliability { Reliable, PartialRexmit, PartialTimers };
    enum class IdSelector { Odd, Even };

    struct MapElement {
        Protocol protocol = Protocol::None;
        uint16_t port     = 0;
        uint16_t streams  = 65535;

        MapElement() = default;
        inline MapElement(Protocol protocol, uint16_t port, uint16_t streams) :
            protocol(protocol), port(port), streams(streams)
        {
        }
        inline MapElement(const QDomElement &el) { parse(el); }
        bool        isValid() const { return protocol != Protocol::None; }
        QDomElement toXml(QDomDocument *doc) const;
        bool        parse(const QDomElement &el);
    };

    struct ChannelElement {
        uint16_t id                = 0;
        uint16_t maxPacketLifeTime = 0;
        uint16_t maxRetransmits    = 0;
        bool     negotiated        = false;
        bool     ordered           = true;
        QString  protocol;

        QDomElement toXml(QDomDocument *doc) const;
        bool        parse(const QDomElement &el);
    };

    QString ns();
    QString webrtcDcNs();

    class AssociationPrivate;
    class Association : public QObject {
        Q_OBJECT;

    public:
        Association(QObject *parent);
        ~Association();

        void                   setIdSelector(IdSelector selector);
        QByteArray             readOutgoing();
        void                   writeIncoming(const QByteArray &data);
        int                    pendingOutgoingDatagrams() const;
        int                    pendingChannels() const;
        Connection::Ptr        nextChannel();
        Connection::Ptr        newChannel(Reliability reliable = Reliable, bool ordered = true, quint32 reliability = 0,
                                          quint16 priority = 256, const QString &label = QString(),
                                          const QString &protocol = QString());
        QList<Connection::Ptr> channels() const;
        // call this when dtls connected
        void onTransportConnected();
        void onTransportError(QAbstractSocket::SocketError error);
        void onTransportClosed();

    signals:
        void readyReadOutgoing();
        void newIncomingChannel();

    private:
        std::unique_ptr<AssociationPrivate> d;
    };

}}}

#endif
