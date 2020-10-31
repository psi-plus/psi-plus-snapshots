/*
 * jignle-s5b.h - Jingle SOCKS5 transport
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

#ifndef JINGLE_S5B_H
#define JINGLE_S5B_H

#include "jingle-transport.h"
#include "tcpportreserver.h"

class QHostAddress;
class SocksClient;

namespace XMPP {
class Client;

namespace Jingle { namespace S5B {
    extern const QString NS;

    class Transport;

    class Candidate {
    public:
        enum Type {
            None, // non standard, just a default
            Proxy,
            Tunnel,
            Assisted,
            Direct
        };

        enum { ProxyPreference = 10, TunnelPreference = 110, AssistedPreference = 120, DirectPreference = 126 };

        /**
         * Local candidates states:
         *   Probing      - potential candidate but no ip:port yet. upnp for example
         *   New          - candidate is ready to be sent to remote
         *   Unacked      - candidate is sent to remote but no iq ack yet
         *   Pending      - canidate sent to remote. we have iq ack but no "used" or "error"
         *   Accepted     - we got "candidate-used" for this candidate
         *   Activating   - only for proxy: we activate the proxy
         *   Active       - use this candidate for actual data transfer
         *   Discarded    - we got "candidate-error" so all pending were marked Discarded
         *
         * Remote candidates states:
         *   New          - the candidate waits its turn to start connection probing
         *   Probing      - connection probing
         *   Pending      - connection was successful, but we didn't send candidate-used to remote
         *   Unacked      - connection was successful and we sent candidate-used to remote but no iq ack yet
         *   Accepted     - we sent candidate-used and got iq ack
         *   Activating   - [not used]
         *   Active       - use this candidate for actual data transfer
         *   Discarded    - failed to connect to all remote candidates
         */
        enum State {
            New,
            Probing,
            Pending,
            Unacked,
            Accepted,
            Activating,
            Active,
            Discarded,
        };

        Candidate();
        Candidate(Transport *transport, const QDomElement &el);
        Candidate(const Candidate &other);
        Candidate(Transport *transport, const Jid &proxy, const QString &cid, quint16 localPreference = 0);
        Candidate(Transport *transport, const TcpPortServer::Ptr &server, const QString &cid,
                  quint16 localPreference = 0);
        ~Candidate();
        Candidate &        operator=(const Candidate &other) = default;
        inline bool        isValid() const { return d != nullptr; }
        inline             operator bool() const { return isValid(); }
        Type               type() const;
        static const char *typeText(Type t);
        QString            cid() const;
        Jid                jid() const;
        QString            host() const;
        void               setHost(const QString &host);
        quint16            port() const;
        void               setPort(quint16 port);
        quint16            localPort() const;
        QHostAddress       localAddress() const;
        State              state() const;
        void               setState(State s);
        static const char *stateText(State s);
        quint32            priority() const;

        QDomElement toXml(QDomDocument *doc) const;
        QString     toString() const;
        inline      operator QString() const { return toString(); }

        void               connectToHost(const QString &key, State successState, QObject *callbackContext,
                                         std::function<void(bool)> callback, bool isUdp = false);
        bool               incomingConnection(SocksClient *sc);
        SocksClient *      takeSocksClient();
        void               deleteSocksClient();
        TcpPortServer::Ptr server() const;
        bool               isConnected() const;

        bool        operator==(const Candidate &other) const;
        inline bool operator!=(const Candidate &other) const { return !(*this == other); }

    private:
        class Private;
        friend class Transport;
        QExplicitlySharedDataPointer<Private> d;
    };

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
        OutgoingTransportInfoUpdate takeOutgoingUpdate() override;
        bool                        isValid() const override;
        TransportFeatures           features() const override;

        QString         sid() const;
        QString         directAddr() const;
        Connection::Ptr addChannel(TransportFeatures features = TransportFeatures()) const override;

    private:
        friend class Manager;

        class Private;
        QScopedPointer<Private> d;
    };

    class Pad : public TransportManagerPad {
        Q_OBJECT
        // TODO
    public:
        typedef QSharedPointer<Pad> Ptr;

        Pad(Manager *manager, Session *session);
        QString           ns() const override;
        Session *         session() const override;
        TransportManager *manager() const override;

        QString generateSid() const;
        void    registerSid(const QString &sid);

        inline TcpPortScope *discoScope() const { return _discoScope; }

    private:
        Manager *     _manager;
        Session *     _session;
        TcpPortScope *_discoScope;
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
        TransportManagerPad *                   pad(Session *session) override;

        void closeAll() override;

        QString generateSid(const Jid &remote);
        void    registerSid(const Jid &remote, const QString &sid);

        /**
         * @brief userProxy returns custom (set by user) SOCKS proxy JID
         * @return
         */
        Jid  userProxy() const;
        void setUserProxy(const Jid &jid);

        /**
         * @brief addKeyMapping sets mapping between key/socks hostname used for direct connection and transport.
         *        The key is sha1(sid, initiator full jid, responder full jid)
         * @param key
         * @param transport
         */
        void addKeyMapping(const QString &key, Transport *transport);
        void removeKeyMapping(const QString &key);

    private:
        class Private;
        QScopedPointer<Private> d;
    };
} // namespace S5B
} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_S5B_H
