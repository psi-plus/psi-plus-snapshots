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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * In s5b.cpp we have
 * S5BManager        -> Jingle::S5B::Manager
 * S5BManager::Item  -> Jingle::S5B::Transport
 * S5BManager::Entry -> ???
 *
 */


#ifndef JINGLE_S5B_H
#define JINGLE_S5B_H

#include "jingle.h"

class SocksClient;

namespace XMPP {

class Client;
class S5BServer;

namespace Jingle {
namespace S5B {

extern const QString NS;

class Candidate {
public:
    enum Type {
        None, // non standard, just a default
        Assisted,
        Direct,
        Proxy,
        Tunnel
    };

    Candidate(const QDomElement &el);
    Candidate(const Candidate &other);
    ~Candidate();
    inline bool isValid() const { return d != nullptr; }

private:
    class Private;
    QSharedDataPointer<Private> d;
};

class Manager;
class Transport : public XMPP::Jingle::Transport
{
    Q_OBJECT
public:
    enum Mode {
        Tcp,
        Udp
    };

    Transport(); // intentionally empty constructor. use createOutgoing/createIncoming
    ~Transport();

    void start();
    bool update(const QDomElement &transportEl);
    Jingle::Action outgoingUpdateType() const override;
    QDomElement takeOutgoingUpdate() override;
    bool isValid() const;
    Features features() const;

    QString sid() const;

private:
    friend class Manager;
    static QSharedPointer<XMPP::Jingle::Transport> createOutgoing(const TransportManagerPad::Ptr &pad, const Jid &to, const QString &transportSid);
    static QSharedPointer<XMPP::Jingle::Transport> createIncoming(const TransportManagerPad::Ptr &pad, const Jid &from, const QDomElement &transportEl);

    class Private;
    QScopedPointer<Private> d;
};

class Pad : public TransportManagerPad
{
    Q_OBJECT
    // TODO
public:
    Pad(Manager *manager, Session *session);
    QString ns() const override;
    Session *session() const override;
    TransportManager *manager() const override;
private:
    Manager *_manager;
    Session *_session;
};

class Manager : public TransportManager {
    Q_OBJECT
public:
    Manager(QObject *parent = nullptr);
    ~Manager();

    XMPP::Jingle::Transport::Features features() const override;
    void setJingleManager(XMPP::Jingle::Manager *jm) override;
    QSharedPointer<XMPP::Jingle::Transport> newTransport(const TransportManagerPad::Ptr &pad) override; // outgoing. one have to call Transport::start to collect candidates
    QSharedPointer<XMPP::Jingle::Transport> newTransport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl) override; // incoming
    TransportManagerPad* pad(Session *session) override;

    bool hasTrasport(const Jid &jid, const QString &sid) const;
    void closeAll() override;

    void setServer(S5BServer *serv);
    bool incomingConnection(SocksClient *client, const QString &key); // returns false if key is unknown
private:
    class Private;
    QScopedPointer<Private> d;
};

} // namespace S5B
} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_S5B_H
