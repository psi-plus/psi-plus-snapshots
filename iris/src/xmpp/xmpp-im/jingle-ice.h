/*
 * jignle-ice.h - Jingle SOCKS5 transport
 * Copyright (C) 2019  Sergey Ilinykh
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

#ifndef JINGLE_ICE_H
#define JINGLE_ICE_H

#include "iris/tcpportreserver.h"
#include "iris/xmpp.h"
#include "jingle-transport.h"

class QHostAddress;

namespace XMPP {
class Client;

namespace Jingle { namespace ICE {
    extern const QString NS;

    class Transport;

    class Manager;
    class Transport : public XMPP::Jingle::Transport {
        Q_OBJECT
    public:
        enum Mode { Tcp, Udp };

        Transport(const TransportManagerPad::Ptr &pad, Origin creator);
        ~Transport() override;

        void                        prepare() override;
        void                        start() override;
        bool                        update(const QDomElement &transportEl) override;
        bool                        hasUpdates() const override;
        OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement) override;
        bool                        isValid() const override;
        TransportFeatures           features() const override;
        int                         maxSupportedChannelsPerComponent(TransportFeatures features) const override;

        void                   setComponentsCount(int count) override;
        Connection::Ptr        addChannel(TransportFeatures features, const QString &id, int component = -1) override;
        QList<Connection::Ptr> channels() const override;

    private:
        friend class Manager;

        class Private;
        std::unique_ptr<Private> d;
    };

    class Pad : public TransportManagerPad {
        Q_OBJECT
        // TODO
    public:
        typedef QSharedPointer<Pad> Ptr;

        Pad(Manager *manager, Session *session);
        QString           ns() const override;
        Session          *session() const override;
        TransportManager *manager() const override;
        void              onLocalAccepted() override;

        inline TcpPortScope *discoScope() const { return _discoScope; }

    private:
        Manager      *_manager;
        Session      *_session;
        TcpPortScope *_discoScope;
        bool          _allowGrouping = false;
    };

    class Manager : public TransportManager {
        Q_OBJECT
    public:
        Manager(QObject *parent = nullptr);
        ~Manager() override;

        XMPP::Jingle::TransportFeatures         features() const override;
        void                                    setJingleManager(XMPP::Jingle::Manager *jm) override;
        QSharedPointer<XMPP::Jingle::Transport> newTransport(const TransportManagerPad::Ptr &pad,
                                                             Origin                          creator) override;
        TransportManagerPad                    *pad(Session *session) override;

        QStringList ns() const override;
        QStringList discoFeatures() const override;

        // TODO reimplement closeAll to support old protocols

        /**
         * @brief userProxy returns custom (set by user) SOCKS proxy JID
         * @return
         */
        // Jid  userProxy() const;
        // void setUserProxy(const Jid &jid);

        /**
         * @brief addKeyMapping sets mapping between key/socks hostname used for direct connection and transport.
         *        The key is sha1(sid, initiator full jid, responder full jid)
         * @param key
         * @param transport
         */
        void addKeyMapping(const QString &key, Transport *transport);
        void removeKeyMapping(const QString &key);

        void setBasePort(int port);
        void setExternalAddress(const QString &host);
        void setSelfAddress(const QHostAddress &addr);
        void setStunBindService(const QString &host, int port);
        void setStunRelayUdpService(const QString &host, int port, const QString &user, const QString &pass);
        void setStunRelayTcpService(const QString &host, int port, const XMPP::AdvancedConnector::Proxy &proxy,
                                    const QString &user, const QString &pass);
        // stunProxy() const;

    private:
        friend class Transport;
        class Private;
        std::unique_ptr<Private> d;
    };
} // namespace Ice
} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_ICE_H
