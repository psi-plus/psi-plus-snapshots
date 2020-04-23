/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "icecomponent.h"

#include "iceagent.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"
#include "objectsession.h"
#include "udpportreserver.h"

#include <QTimer>
#include <QUdpSocket>
#include <QUuid>
#include <QtCrypto>
#include <stdlib.h>

namespace XMPP {
static int calc_priority(int typePref, int localPref, int componentId)
{
    Q_ASSERT(typePref >= 0 && typePref <= 126);
    Q_ASSERT(localPref >= 0 && localPref <= 65535);
    Q_ASSERT(componentId >= 1 && componentId <= 256);

    int priority = (1 << 24) * typePref;
    priority += (1 << 8) * localPref;
    priority += (256 - componentId);
    return priority;
}

class IceComponent::Private : public QObject {
    Q_OBJECT

    struct IceSocket {
        using Ptr = QSharedPointer<IceSocket>;

        IceSocket(QUdpSocket *sock, bool borrowed, IceComponent::Private *ic) : sock(sock), ic(ic), borrowed(borrowed)
        {
        }
        ~IceSocket()
        {
            if (!sock)
                return;

            sock->disconnect(ic);
            if (borrowed) {
                Q_ASSERT(ic->portReserver);
                ic->portReserver->returnSockets(QList<QUdpSocket *>() << sock);
            } else {
                sock->deleteLater();
            }
        }

        QUdpSocket *           sock = nullptr;
        IceComponent::Private *ic   = nullptr;
        bool                   borrowed;
    };

public:
    class Config {
    public:
        QList<Ice176::LocalAddress> localAddrs;

        // for example manually provided external address mapped to every local
        QList<Ice176::ExternalAddress> extAddrs;

        QHostAddress stunBindAddr;
        int          stunBindPort;

        QHostAddress     stunRelayUdpAddr;
        int              stunRelayUdpPort;
        QString          stunRelayUdpUser;
        QCA::SecureArray stunRelayUdpPass;

        QHostAddress     stunRelayTcpAddr;
        int              stunRelayTcpPort;
        QString          stunRelayTcpUser;
        QCA::SecureArray stunRelayTcpPass;
    };

    class LocalTransport {
    public:
        IceSocket::Ptr                    qsock;
        QHostAddress                      addr;
        QSharedPointer<IceLocalTransport> sock;
        int                               network;
        bool                              isVpn;
        bool                              started;
        bool                              stun_started;
        bool                              stun_finished, turn_finished; // candidates emitted
        QHostAddress                      extAddr;
        bool                              ext_finished;

        LocalTransport() :
            network(-1), isVpn(false), started(false), stun_started(false), stun_finished(false), turn_finished(false),
            ext_finished(false)
        {
        }
    };

    IceComponent *                     q;
    ObjectSession                      sess;
    int                                id;
    QString                            clientSoftware;
    TurnClient::Proxy                  proxy;
    UdpPortReserver *                  portReserver = nullptr;
    Config                             pending;
    Config                             config;
    bool                               stopping = false;
    QList<LocalTransport *>            udpTransports; // transport for local host-only candidates
    QSharedPointer<IceTurnTransport>   tcpTurn;       // tcp relay candidate
    QList<Candidate>                   localCandidates;
    QHash<int, QSet<TransportAddress>> channelPeers;
    bool                               useLocal        = true; // use local host candidates
    bool                               useStunBind     = true;
    bool                               useStunRelayUdp = true;
    bool                               useStunRelayTcp = true;
    bool                               localFinished   = false;
    // bool                               stunFinished      = false;
    bool gatheringComplete = false;
    int  debugLevel        = DL_None;

    Private(IceComponent *_q) : QObject(_q), q(_q), sess(this) { }

    ~Private() { qDeleteAll(udpTransports); }

    LocalTransport *createLocalTransport(IceSocket::Ptr socket, const Ice176::LocalAddress &la)
    {
        auto lt   = new LocalTransport;
        lt->qsock = socket;
        lt->addr  = la.addr;
        lt->sock  = QSharedPointer<IceLocalTransport>::create();
        lt->sock->setDebugLevel(IceTransport::DebugLevel(debugLevel));
        lt->network = la.network;
        lt->isVpn   = la.isVpn;
        connect(lt->sock.data(), SIGNAL(started()), SLOT(lt_started()));
        connect(lt->sock.data(), SIGNAL(stopped()), SLOT(lt_stopped()));
        connect(lt->sock.data(), SIGNAL(addressesChanged()), SLOT(lt_addressesChanged()));
        connect(lt->sock.data(), SIGNAL(error(int)), SLOT(lt_error(int)));
        connect(lt->sock.data(), SIGNAL(debugLine(QString)), SLOT(lt_debugLine(QString)));
        return lt;
    }

    void update(QList<QUdpSocket *> *socketList)
    {
        Q_ASSERT(!stopping);

        // only allow setting stun stuff once
        if (!pending.stunBindAddr.isNull() && config.stunBindAddr.isNull()) {
            config.stunBindAddr     = pending.stunBindAddr;
            config.stunBindPort     = pending.stunBindPort;
            config.stunRelayUdpAddr = pending.stunRelayUdpAddr;
            config.stunRelayUdpPort = pending.stunRelayUdpPort;
            config.stunRelayUdpUser = pending.stunRelayUdpUser;
            config.stunRelayUdpPass = pending.stunRelayUdpPass;
            config.stunRelayTcpAddr = pending.stunRelayTcpAddr;
            config.stunRelayTcpPort = pending.stunRelayTcpPort;
            config.stunRelayTcpUser = pending.stunRelayTcpUser;
            config.stunRelayTcpPass = pending.stunRelayTcpPass;
        }

        // for now, only allow setting localAddrs once
        if (!pending.localAddrs.isEmpty() && config.localAddrs.isEmpty()) {
            for (const Ice176::LocalAddress &la : pending.localAddrs) {
                // skip duplicate addrs
                if (findLocalAddr(la.addr) != -1)
                    continue;

                QUdpSocket *qsock = nullptr;
                if (useLocal && socketList) {
                    qsock = takeFromSocketList(socketList, la.addr, this);
                }
                bool borrowedSocket = qsock != nullptr;
                if (!qsock) {
                    // otherwise, bind to random
                    qsock = new QUdpSocket(this);
                    if (!qsock->bind(la.addr, 0)) {
                        delete qsock;
                        emit q->debugLine("Warning: unable to bind to random port.");
                        continue;
                    }
                }

                config.localAddrs += la;
                auto lt = createLocalTransport(IceSocket::Ptr::create(qsock, borrowedSocket, this), la);
                udpTransports += lt;

                if (useStunBind && !config.stunBindAddr.isNull()) {
                    lt->sock->setStunBindService(config.stunBindAddr, config.stunBindPort);
                }
                if (useStunRelayUdp && !config.stunRelayUdpAddr.isNull() && !config.stunRelayUdpUser.isEmpty()) {
                    lt->sock->setStunRelayService(config.stunRelayUdpAddr, config.stunRelayUdpPort,
                                                  config.stunRelayUdpUser, config.stunRelayUdpPass);
                }

                int port = qsock->localPort();
                lt->sock->start(qsock);
                emit q->debugLine(QString("starting transport ") + la.addr.toString() + ';' + QString::number(port)
                                  + " for component " + QString::number(id));
            }
        }

        // extAddrs created on demand if present, but only once
        if (!pending.extAddrs.isEmpty() && config.extAddrs.isEmpty()) {
            config.extAddrs = pending.extAddrs;

            bool need_doExt = false;

            for (auto lt : udpTransports) {
                // already assigned an ext address?  skip
                if (!lt->extAddr.isNull())
                    continue;

                QHostAddress laddr = lt->sock->localAddress();
                int          lport = lt->sock->localPort();

                if (laddr.protocol() == QAbstractSocket::IPv6Protocol)
                    continue;

                // find external address by address of local socket (external has to be configured that way)
                auto eaIt = std::find_if(config.extAddrs.constBegin(), config.extAddrs.constEnd(), [&](auto const &ea) {
                    return ea.base.addr == laddr && (ea.portBase == -1 || ea.portBase == lport);
                });

                if (eaIt != config.extAddrs.constEnd()) {
                    lt->extAddr = eaIt->addr;
                    if (lt->started)
                        need_doExt = true;
                }
            }

            if (need_doExt)
                QTimer::singleShot(0, this, [this]() {
                    if (stopping)
                        return;

                    ObjectSessionWatcher watch(&sess);

                    for (auto lt : udpTransports) {
                        if (lt->started) {
                            int addrAt = findLocalAddr(lt->addr);
                            Q_ASSERT(addrAt != -1);

                            ensureExt(lt, addrAt); // will emit candidateAdded if everything goes well
                            if (!watch.isValid())
                                return;
                        }
                    }
                });
        }

        if (useStunRelayTcp && !config.stunRelayTcpAddr.isNull() && !config.stunRelayTcpUser.isEmpty() && !tcpTurn) {
            tcpTurn = QSharedPointer<IceTurnTransport>::create();
            tcpTurn->setDebugLevel(IceTransport::DebugLevel(debugLevel));
            connect(tcpTurn.data(), SIGNAL(started()), SLOT(tt_started()));
            connect(tcpTurn.data(), SIGNAL(stopped()), SLOT(tt_stopped()));
            connect(tcpTurn.data(), SIGNAL(error(int)), SLOT(tt_error(int)));
            connect(tcpTurn.data(), SIGNAL(debugLine(QString)), SLOT(tt_debugLine(QString)));
            tcpTurn->setClientSoftwareNameAndVersion(clientSoftware);
            tcpTurn->setProxy(proxy);
            tcpTurn->setUsername(config.stunRelayTcpUser);
            tcpTurn->setPassword(config.stunRelayTcpPass);
            tcpTurn->start(config.stunRelayTcpAddr, config.stunRelayTcpPort);

            emit q->debugLine(QString("starting TURN transport with server ") + config.stunRelayTcpAddr.toString() + ';'
                              + QString::number(config.stunRelayTcpPort) + " for component " + QString::number(id));
        }

        if (udpTransports.isEmpty() && !localFinished) {
            localFinished = true;
            sess.defer(q, "localFinished");
        }
        sess.defer(this, "tryGatheringComplete");
    }

    void stop()
    {
        Q_ASSERT(!stopping);

        stopping = true;

        // nothing to stop?
        if (allStopped()) {
            sess.defer(this, "postStop");
            return;
        }

        for (LocalTransport *lt : udpTransports)
            lt->sock->stop();

        if (tcpTurn)
            tcpTurn->stop();
    }

    int peerReflexivePriority(QSharedPointer<IceTransport> iceTransport, int path) const
    {
        int                      addrAt = -1;
        const IceLocalTransport *lt     = qobject_cast<const IceLocalTransport *>(iceTransport.data());
        if (lt) {
            auto it = std::find_if(udpTransports.begin(), udpTransports.end(),
                                   [&](auto const &a) { return a->sock == lt; });
            Q_ASSERT(it != udpTransports.end());
            addrAt = std::distance(udpTransports.begin(), it);
            if (path == 1) {
                // lower priority, but not as far as IceTurnTransport
                addrAt += 512;
            }
        } else if (qobject_cast<const IceTurnTransport *>(iceTransport) == tcpTurn) {
            // lower priority by making it seem like the last nic
            addrAt = 1024;
        }

        return choose_default_priority(PeerReflexiveType, 65535 - addrAt, false, id);
    }

    void flagPathAsLowOverhead(int id, const QHostAddress &addr, int port)
    {
        int at = -1;
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].id == id) {
                at = n;
                break;
            }
        }

        Q_ASSERT(at != -1);

        if (at == -1)
            return;

        Candidate &c = localCandidates[at];

        TransportAddress        ta(addr, port);
        QSet<TransportAddress> &addrs = channelPeers[c.id];
        if (!addrs.contains(ta)) {
            addrs += ta;
            c.iceTransport->addChannelPeer(ta.addr, ta.port);
        }
    }

    void addLocalPeerReflexiveCandidate(const IceComponent::TransportAddress &addr,
                                        const IceComponent::CandidateInfo &base, quint32 priority)
    {
        IceComponent::CandidateInfo ci;
        ci.addr = addr;
        ci.addr.addr.setScopeId(QString());
        ci.base        = base.addr;
        ci.type        = IceComponent::PeerReflexiveType;
        ci.priority    = priority;
        ci.foundation  = IceAgent::instance()->foundation(IceComponent::PeerReflexiveType, ci.base.addr);
        ci.componentId = base.componentId;
        ci.network     = base.network;

        auto baseCand = std::find_if(localCandidates.begin(), localCandidates.end(), [&](auto const &c) {
            return c.info.base == base.base && c.info.type == HostType;
        });
        Q_ASSERT(baseCand != localCandidates.end());

        Candidate c;
        c.id           = getId();
        c.info         = ci;
        c.iceTransport = baseCand->iceTransport;
        c.path         = 0;

        localCandidates += c;

        emit q->candidateAdded(c);
    }

private:
    // localPref is the priority of the network interface being used for
    //   this candidate.  the value must be between 0-65535 and different
    //   interfaces must have different values.  if there is only one
    //   interface, the value should be 65535.
    static int choose_default_priority(CandidateType type, int localPref, bool isVpn, int componentId)
    {
        int typePref;
        if (type == HostType) {
            if (isVpn)
                typePref = 0;
            else
                typePref = 126;
        } else if (type == PeerReflexiveType)
            typePref = 110;
        else if (type == ServerReflexiveType)
            typePref = 100;
        else // RelayedType
            typePref = 0;

        return calc_priority(typePref, localPref, componentId);
    }

    static QUdpSocket *takeFromSocketList(QList<QUdpSocket *> *socketList, const QHostAddress &addr,
                                          QObject *parent = nullptr)
    {
        for (int n = 0; n < socketList->count(); ++n) {
            if ((*socketList)[n]->localAddress() == addr) {
                QUdpSocket *sock = socketList->takeAt(n);
                sock->setParent(parent);
                return sock;
            }
        }

        return nullptr;
    }

    int getId() const
    {
        for (int n = 0;; ++n) {
            bool found = false;
            foreach (const Candidate &c, localCandidates) {
                if (c.id == n) {
                    found = true;
                    break;
                }
            }

            if (!found)
                return n;
        }
    }

    int findLocalAddr(const QHostAddress &addr)
    {
        for (int n = 0; n < config.localAddrs.count(); ++n) {
            if (config.localAddrs[n].addr == addr)
                return n;
        }

        return -1;
    }

    void ensureExt(LocalTransport *lt, int addrAt)
    {
        if (!lt->extAddr.isNull() && !lt->ext_finished) {
            CandidateInfo ci;
            ci.addr.addr   = lt->extAddr;
            ci.addr.port   = lt->sock->localPort();
            ci.type        = ServerReflexiveType;
            ci.componentId = id;
            ci.priority    = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
            ci.base.addr   = lt->sock->localAddress();
            ci.base.port   = lt->sock->localPort();
            ci.network     = lt->network;
            ci.foundation  = IceAgent::instance()->foundation(ServerReflexiveType, ci.base.addr);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = lt->sock;
            c.path         = 0;

            lt->ext_finished = true;

            storeLocalNotReduntantCandidate(c);
        }
    }

    void removeLocalCandidates(const QSharedPointer<IceTransport> sock)
    {
        ObjectSessionWatcher watch(&sess);

        for (int n = 0; n < localCandidates.count(); ++n) {
            Candidate &c = localCandidates[n];

            if (c.iceTransport == sock) {
                Candidate tmp = localCandidates.takeAt(n);
                --n; // adjust position

                channelPeers.remove(tmp.id);

                emit q->candidateRemoved(tmp);
                if (!watch.isValid())
                    return;
            }
        }
    }

    void storeLocalNotReduntantCandidate(const Candidate &c)
    {
        ObjectSessionWatcher watch(&sess);
        // RFC8445 5.1.3.  Eliminating Redundant Candidates
        auto it = std::find_if(localCandidates.begin(), localCandidates.end(), [&](const Candidate &cc) {
            return cc.info.addr == c.info.addr && cc.info.base == c.info.base && cc.info.priority >= c.info.priority;
        });
        if (it == localCandidates.end()) { // not reduntant
            localCandidates += c;
            emit q->candidateAdded(c);
        }
    }

    bool allStopped() const { return udpTransports.isEmpty() && !tcpTurn; }

    void tryStopped()
    {
        if (allStopped())
            postStop();
    }

private slots:
    void tryGatheringComplete()
    {
        if (gatheringComplete || (tcpTurn && !tcpTurn->isStarted()))
            return;

        auto checkFinished = [&](const LocalTransport *lt) {
            return lt->started && (lt->sock->stunBindServiceAddress().isNull() || lt->stun_finished)
                && (lt->sock->stunRelayServiceAddress().isNull() || lt->turn_finished);
        };

        bool allFinished = true;
        for (const LocalTransport *lt : udpTransports) {
            if (!checkFinished(lt)) {
                allFinished = false;
                break;
            }
        }

        if (allFinished) {
            gatheringComplete = true;
            emit q->gatheringComplete();
        }
    }

    void postStop()
    {
        stopping = false;

        emit q->stopped();
    }

    void lt_started()
    {
        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());

        auto it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });
        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        lt->started = true;

        int addrAt = findLocalAddr(lt->addr);
        Q_ASSERT(addrAt != -1);

        ObjectSessionWatcher watch(&sess);

        if (useLocal) {
            CandidateInfo ci;
            ci.addr.addr   = lt->sock->localAddress();
            ci.addr.port   = lt->sock->localPort();
            ci.type        = HostType;
            ci.componentId = id;
            ci.priority    = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
            ci.base        = ci.addr;
            ci.network     = lt->network;
            ci.foundation  = IceAgent::instance()->foundation(HostType, ci.base.addr);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 0;

            localCandidates += c;

            emit q->candidateAdded(c);
            if (!watch.isValid())
                return;

            ensureExt(lt, addrAt);
            if (!watch.isValid())
                return;
        }

        // setup stun/turn
        bool bind  = useStunBind && !config.stunBindAddr.isNull();
        bool relay = useStunRelayUdp && !config.stunRelayUdpAddr.isNull() && !config.stunRelayUdpUser.isEmpty();
        // if need to start stun/turn
        if (!lt->stun_started && (bind || relay) && lt->addr.protocol() != QAbstractSocket::IPv6Protocol) {
            lt->sock->setClientSoftwareNameAndVersion(clientSoftware);
            if (bind) {
                lt->sock->setStunBindService(config.stunBindAddr, config.stunBindPort);
            }
            if (relay) {
                lt->sock->setStunRelayService(config.stunRelayUdpAddr, config.stunRelayUdpPort, config.stunRelayUdpUser,
                                              config.stunRelayUdpPass);
            }
            lt->stun_started = true;
            lt->sock->stunStart();
            if (!watch.isValid())
                return;
        }

        // check completeness of various stuff
        if (!localFinished) {
            bool allStarted = true;
            for (const LocalTransport *lt : udpTransports) {
                if (!lt->started) {
                    allStarted = false;
                    break;
                }
            }
            if (allStarted) {
                localFinished = true;
                emit q->localFinished();
                if (!watch.isValid())
                    return;
            }
        }

        tryGatheringComplete();
    }

    void lt_stopped()
    {
        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());
        auto               it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });
        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        ObjectSessionWatcher watch(&sess);

        removeLocalCandidates(lt->sock);
        if (!watch.isValid())
            return;

        lt->sock->disconnect(this);
        delete lt;

        udpTransports.erase(it);
        tryStopped();
    }

    void lt_addressesChanged()
    {
        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());
        auto               it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });

        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        int addrAt = findLocalAddr(lt->addr);
        Q_ASSERT(addrAt != -1);

        ObjectSessionWatcher watch(&sess);

        if (useStunBind && !lt->sock->serverReflexiveAddress().isNull() && !lt->stun_finished) {
            // automatically assign ext to related leaps, if possible
            for (LocalTransport *i : udpTransports) {
                if (i->extAddr.isNull() && i->sock->localAddress() == lt->sock->localAddress()) {
                    i->extAddr = lt->sock->serverReflexiveAddress();
                    if (i->started) {
                        ensureExt(i, addrAt);
                        if (!watch.isValid())
                            return;
                    }
                }
            }

            CandidateInfo ci;
            ci.addr.addr   = lt->sock->serverReflexiveAddress();
            ci.addr.port   = lt->sock->serverReflexivePort();
            ci.base.addr   = lt->sock->localAddress();
            ci.base.port   = lt->sock->localPort();
            ci.type        = ServerReflexiveType;
            ci.componentId = id;
            ci.priority    = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
            ci.network     = lt->network;
            ci.foundation  = IceAgent::instance()->foundation(
                ServerReflexiveType, ci.base.addr, lt->sock->reflexiveAddressSource(), QAbstractSocket::UdpSocket);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 0;

            lt->stun_finished = true;

            storeLocalNotReduntantCandidate(c);
        }

        if (!lt->sock->relayedAddress().isNull() && !lt->turn_finished) {
            CandidateInfo ci;
            ci.addr.addr   = lt->sock->relayedAddress();
            ci.addr.port   = lt->sock->relayedPort();
            ci.base.addr   = lt->sock->relayedAddress();
            ci.base.port   = lt->sock->relayedPort();
            ci.type        = RelayedType;
            ci.componentId = id;
            ci.priority    = choose_default_priority(ci.type, 65535 - addrAt, lt->isVpn, ci.componentId);
            ci.network     = lt->network;
            ci.foundation  = IceAgent::instance()->foundation(
                RelayedType, ci.base.addr, lt->sock->stunRelayServiceAddress(), QAbstractSocket::UdpSocket);

            Candidate c;
            c.id           = getId();
            c.info         = ci;
            c.iceTransport = sock->sharedFromThis();
            c.path         = 1;

            lt->turn_finished = true;

            storeLocalNotReduntantCandidate(c);
        }
        if (!watch.isValid())
            return;

        tryGatheringComplete();
    }

    void lt_error(int e)
    {
        Q_UNUSED(e)

        IceLocalTransport *sock = static_cast<IceLocalTransport *>(sender());
        auto               it
            = std::find_if(udpTransports.begin(), udpTransports.end(), [&](auto const &a) { return a->sock == sock; });

        Q_ASSERT(it != udpTransports.end());
        LocalTransport *lt = *it;

        ObjectSessionWatcher watch(&sess);

        removeLocalCandidates(lt->sock);
        if (!watch.isValid())
            return;

        lt->sock->disconnect(this);
        delete lt;

        udpTransports.erase(it);

        tryGatheringComplete();
    }

    void lt_debugLine(const QString &line) { emit q->debugLine(line); }

    void tt_started()
    {
        // lower priority by making it seem like the last nic
        int addrAt = 1024;

        CandidateInfo ci;
        ci.addr.addr   = tcpTurn->relayedAddress();
        ci.addr.port   = tcpTurn->relayedPort();
        ci.type        = RelayedType;
        ci.componentId = id;
        ci.priority    = choose_default_priority(ci.type, 65535 - addrAt, false, ci.componentId);
        ci.base        = ci.addr;
        ci.network     = 0; // not relevant
        ci.foundation  = IceAgent::instance()->foundation(RelayedType, ci.base.addr, config.stunRelayTcpAddr,
                                                         QAbstractSocket::TcpSocket);

        Candidate c;
        c.id           = getId();
        c.info         = ci;
        c.iceTransport = tcpTurn->sharedFromThis();
        c.path         = 0;

        localCandidates += c;

        emit q->candidateAdded(c);

        tryGatheringComplete();
    }

    void tt_stopped()
    {
        ObjectSessionWatcher watch(&sess);

        removeLocalCandidates(tcpTurn->sharedFromThis());
        if (!watch.isValid())
            return;

        tcpTurn->disconnect(this);
        tcpTurn.reset();

        tryStopped();
    }

    void tt_error(int e)
    {
        Q_UNUSED(e)

        ObjectSessionWatcher watch(&sess);

        removeLocalCandidates(tcpTurn);
        if (!watch.isValid())
            return;

        tcpTurn->disconnect(this);
        tcpTurn.reset();
        tryGatheringComplete();
    }

    void tt_debugLine(const QString &line) { emit q->debugLine(line); }
};

IceComponent::IceComponent(int id, QObject *parent) : QObject(parent)
{
    d     = new Private(this);
    d->id = id;
}

IceComponent::~IceComponent() { delete d; }

int IceComponent::id() const { return d->id; }

bool IceComponent::isGatheringComplete() const { return d->gatheringComplete; }

void IceComponent::setClientSoftwareNameAndVersion(const QString &str) { d->clientSoftware = str; }

void IceComponent::setProxy(const TurnClient::Proxy &proxy) { d->proxy = proxy; }

void IceComponent::setPortReserver(UdpPortReserver *portReserver) { d->portReserver = portReserver; }

UdpPortReserver *IceComponent::portReserver() const { return d->portReserver; }

void IceComponent::setLocalAddresses(const QList<Ice176::LocalAddress> &addrs) { d->pending.localAddrs = addrs; }

void IceComponent::setExternalAddresses(const QList<Ice176::ExternalAddress> &addrs) { d->pending.extAddrs = addrs; }

void IceComponent::setStunBindService(const QHostAddress &addr, int port)
{
    d->pending.stunBindAddr = addr;
    d->pending.stunBindPort = port;
}

void IceComponent::setStunRelayUdpService(const QHostAddress &addr, int port, const QString &user,
                                          const QCA::SecureArray &pass)
{
    d->pending.stunRelayUdpAddr = addr;
    d->pending.stunRelayUdpPort = port;
    d->pending.stunRelayUdpUser = user;
    d->pending.stunRelayUdpPass = pass;
}

void IceComponent::setStunRelayTcpService(const QHostAddress &addr, int port, const QString &user,
                                          const QCA::SecureArray &pass)
{
    d->pending.stunRelayTcpAddr = addr;
    d->pending.stunRelayTcpPort = port;
    d->pending.stunRelayTcpUser = user;
    d->pending.stunRelayTcpPass = pass;
}

void IceComponent::setUseLocal(bool enabled) { d->useLocal = enabled; }

void IceComponent::setUseStunBind(bool enabled) { d->useStunBind = enabled; }

void IceComponent::setUseStunRelayUdp(bool enabled) { d->useStunRelayUdp = enabled; }

void IceComponent::setUseStunRelayTcp(bool enabled) { d->useStunRelayTcp = enabled; }

void IceComponent::update(QList<QUdpSocket *> *socketList) { d->update(socketList); }

void IceComponent::stop() { d->stop(); }

int IceComponent::peerReflexivePriority(QSharedPointer<IceTransport> iceTransport, int path) const
{
    return d->peerReflexivePriority(iceTransport, path);
}

void IceComponent::addLocalPeerReflexiveCandidate(const IceComponent::TransportAddress &addr,
                                                  const IceComponent::CandidateInfo &base, quint32 priority)
{
    d->addLocalPeerReflexiveCandidate(addr, base, priority);
}

void IceComponent::flagPathAsLowOverhead(int id, const QHostAddress &addr, int port)
{
    return d->flagPathAsLowOverhead(id, addr, port);
}

void IceComponent::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    foreach (const Private::LocalTransport *lt, d->udpTransports)
        lt->sock->setDebugLevel(IceTransport::DebugLevel(level));
    if (d->tcpTurn)
        d->tcpTurn->setDebugLevel((IceTransport::DebugLevel)level);
}

IceComponent::CandidateInfo IceComponent::CandidateInfo::makeRemotePrflx(int componentId, const QHostAddress &fromAddr,
                                                                         quint16 fromPort, quint32 priority)
{
    IceComponent::CandidateInfo c;
    c.addr = TransportAddress(fromAddr, fromPort);
    c.addr.addr.setScopeId(QString());
    c.type        = PeerReflexiveType;
    c.priority    = priority;
    c.foundation  = QUuid::createUuid().toString();
    c.componentId = componentId;
    c.network     = -1;
    return c;
}

} // namespace XMPP

#include "icecomponent.moc"
