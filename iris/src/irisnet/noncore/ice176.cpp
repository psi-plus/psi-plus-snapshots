/*
 * Copyright (C) 2009-2010  Barracuda Networks, Inc.
 * Copyright (C) 2020  Sergey Ilinykh
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

#include "ice176.h"

#include "iceagent.h"
#include "icecomponent.h"
#include "icelocaltransport.h"
#include "iceturntransport.h"
#include "stunbinding.h"
#include "stunmessage.h"
#include "stuntransaction.h"
#include "stuntypes.h"
#include "udpportreserver.h"

#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QtCrypto>

namespace XMPP {
enum { Direct, Relayed };

static qint64 calc_pair_priority(int a, int b)
{
    qint64 priority = ((qint64)1 << 32) * qMin(a, b);
    priority += (qint64)2 * qMax(a, b);
    if (a > b)
        ++priority;
    return priority;
}

// scope values: 0 = local, 1 = link-local, 2 = private, 3 = public
// FIXME: dry (this is in psi avcall also)
static int getAddressScope(const QHostAddress &a)
{
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        if (a == QHostAddress(QHostAddress::LocalHostIPv6))
            return 0;
        else if (XMPP::Ice176::isIPv6LinkLocalAddress(a))
            return 1;
    } else if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        quint32 v4 = a.toIPv4Address();
        quint8  a0 = v4 >> 24;
        quint8  a1 = (v4 >> 16) & 0xff;
        if (a0 == 127)
            return 0;
        else if (a0 == 169 && a1 == 254)
            return 1;
        else if (a0 == 10)
            return 2;
        else if (a0 == 172 && a1 >= 16 && a1 <= 31)
            return 2;
        else if (a0 == 192 && a1 == 168)
            return 2;
    }

    return 3;
}

class Ice176::Private : public QObject {
    Q_OBJECT

public:
    // note, Nominating state is skipped when aggressive nomination is enabled.
    enum State {
        Stopped,
        Starting,   // preparing local candidates right after start() call
        Started,    // local candidates ready. ready for pairing with remote
        Nominating, // for ice2 it's a state when we decided to nominate one of active pairs
        Active,     // all components have a nominated pair and media transferred over them
        Stopping    // when received a command from the user to stop
    };

    enum CandidatePairState { PWaiting, PInProgress, PSucceeded, PFailed, PFrozen };

    enum CheckListState { LRunning, LCompleted, LFailed };

    class CandidatePair {
    public:
        using Ptr = QSharedPointer<CandidatePair>;

        IceComponent::CandidateInfo::Ptr local, remote;
        bool                             isDefault   = false; // not used in xmpp
        bool                             isValid     = false; // a pair which is also in valid list
        bool                             isNominated = false;

        // states for last or comming checks
        bool isTriggered             = false; // last scheduled check was a triggered check
        bool isTriggeredForNominated = false;
        bool finalNomination         = false;

        CandidatePairState state = CandidatePairState::PFrozen;

        qint64  priority = 0;
        QString foundation; // rfc8445 6.1.2.6 (combination of foundations)

        StunBinding *binding = nullptr;

        // FIXME: this is wrong i think, it should be in LocalTransport
        //   or such, to multiplex ids
        StunTransactionPool *pool = nullptr;

        inline bool isNull() const { return local->addr.addr.isNull() || remote->addr.addr.isNull(); }
        inline      operator QString() const
        {
            if (isNull())
                return QLatin1String("null pair");
            return QString(QLatin1String("L:\"%1\" %2 - R:\"%3\" %4"))
                .arg(candidateType_to_string(local->type), QString(local->addr), candidateType_to_string(remote->type),
                     QString(remote->addr));
        }
    };

    class CheckList {
    public:
        QList<QSharedPointer<CandidatePair>> pairs;
        QQueue<QWeakPointer<CandidatePair>>  triggeredPairs;
        QList<QSharedPointer<CandidatePair>> validPairs; // highest priority and nominated come first
        CheckListState                       state;
    };

    class Component {
    public:
        int                     id              = 0;
        IceComponent *          ic              = nullptr;
        std::unique_ptr<QTimer> nominationTimer = std::unique_ptr<QTimer>();
        bool                    localFinished   = false;
        bool                    hasValidPairs   = false;
        bool                    finalSelected   = false;
        bool                    stopped         = false;
        bool                    lowOverhead     = false;
    };

    Ice176 *                                q;
    Ice176::Mode                            mode;
    State                                   state = Stopped;
    QTimer                                  checkTimer;
    TurnClient::Proxy                       proxy;
    UdpPortReserver *                       portReserver = nullptr;
    std::unique_ptr<QTimer>                 remoteGatheringCompleteTimer;
    int                                     agressiveNominationTimeout     = 3000;  // 3s
    int                                     remoteGatheringCompleteTimeout = 30000; // 30s
    int                                     componentCount                 = 0;
    QList<Ice176::LocalAddress>             localAddrs;
    QList<Ice176::ExternalAddress>          extAddrs;
    QHostAddress                            stunBindAddr;
    int                                     stunBindPort;
    QHostAddress                            stunRelayUdpAddr;
    int                                     stunRelayUdpPort;
    QString                                 stunRelayUdpUser;
    QCA::SecureArray                        stunRelayUdpPass;
    QHostAddress                            stunRelayTcpAddr;
    int                                     stunRelayTcpPort;
    QString                                 stunRelayTcpUser;
    QCA::SecureArray                        stunRelayTcpPass;
    QString                                 localUser, localPass;
    QString                                 peerUser, peerPass;
    std::vector<Component>                  components;
    QList<IceComponent::Candidate>          localCandidates;
    QList<IceComponent::CandidateInfo::Ptr> remoteCandidates;
    QSet<QWeakPointer<IceTransport>>        iceTransports;
    CheckList                               checkList;
    QList<QList<QByteArray>>                in;
    Features                                remoteFeatures;
    Features                                localFeatures;
    bool                                    useLocal                   = true;
    bool                                    useStunBind                = true;
    bool                                    useStunRelayUdp            = true;
    bool                                    useStunRelayTcp            = true;
    bool                                    localHostGatheringFinished = false;
    bool                                    localGatheringComplete     = false;
    bool                                    remoteGatheringComplete    = false;
    bool                                    readyToSendMedia           = false;

    Private(Ice176 *_q) : QObject(_q), q(_q)
    {
        connect(&checkTimer, &QTimer::timeout, this, [this]() {
            auto pair = selectNextPairToCheck();
            if (pair)
                checkPair(pair);
        });
        checkTimer.setInterval(20);
        checkTimer.setSingleShot(false);
    }

    ~Private()
    {
        for (const Component &c : components)
            delete c.ic;

        // no need to delete pools and bindings since pools already deleted here
        // by QObject destructor as children of this(Ice176::Private) object.
        // Bindings deleted too as children of pool
        // should be reviewed by Justin =)
        /*for(int n = 0; n < checkList.pairs.count(); ++n)
        {
            StunBinding *binding = checkList.pairs[n].binding;
            StunTransactionPool *pool = checkList.pairs[n].pool;

            delete binding;

            if(pool)
            {
                pool->disconnect(this);
                pool->setParent(0);
                pool->deleteLater();
            }
        }*/
    }

    void reset() { checkTimer.stop(); /*TODO*/ }

    int findLocalAddress(const QHostAddress &addr)
    {
        for (int n = 0; n < localAddrs.count(); ++n) {
            if (localAddrs[n].addr == addr)
                return n;
        }

        return -1;
    }

    void updateLocalAddresses(const QList<LocalAddress> &addrs)
    {
        // for now, ignore address changes during operation
        if (state != Stopped)
            return;

        localAddrs.clear();
        for (const auto &la : addrs) {
            int at = findLocalAddress(la.addr);
            if (at == -1)
                localAddrs += la;
        }
    }

    void updateExternalAddresses(const QList<ExternalAddress> &addrs)
    {
        // for now, ignore address changes during operation
        if (state != Stopped)
            return;

        extAddrs.clear();
        for (const ExternalAddress &ea : addrs) {
            int at = findLocalAddress(ea.base.addr);
            if (at != -1)
                extAddrs += ea;
        }
    }

    void start()
    {
        Q_ASSERT(state == Stopped);

        state = Starting;

        localUser = IceAgent::randomCredential(4);
        localPass = IceAgent::randomCredential(22);

        if (!useLocal)
            useStunBind = false;

        QList<QUdpSocket *> socketList;
        if (portReserver)
            // list size = componentCount * number of interfaces
            socketList = portReserver->borrowSockets(componentCount, this);

        components.reserve(componentCount);
        for (int n = 0; n < componentCount; ++n) {
            components.emplace_back();
            Component &c = components.back();
            c.id         = n + 1;
            c.ic         = new IceComponent(c.id, this);
            c.ic->setDebugLevel(IceComponent::DL_Info);
            connect(c.ic, SIGNAL(candidateAdded(XMPP::IceComponent::Candidate)),
                    SLOT(ic_candidateAdded(XMPP::IceComponent::Candidate)));
            connect(c.ic, SIGNAL(candidateRemoved(XMPP::IceComponent::Candidate)),
                    SLOT(ic_candidateRemoved(XMPP::IceComponent::Candidate)));
            connect(c.ic, SIGNAL(localFinished()), SLOT(ic_localFinished()));
            connect(c.ic, &IceComponent::gatheringComplete, this, &Private::ic_gatheringComplete);
            connect(c.ic, SIGNAL(stopped()), SLOT(ic_stopped()));
            connect(c.ic, SIGNAL(debugLine(QString)), SLOT(ic_debugLine(QString)));

            c.ic->setClientSoftwareNameAndVersion("Iris");
            c.ic->setProxy(proxy);
            if (portReserver)
                c.ic->setPortReserver(portReserver);
            c.ic->setLocalAddresses(localAddrs);
            c.ic->setExternalAddresses(extAddrs);
            if (!stunBindAddr.isNull())
                c.ic->setStunBindService(stunBindAddr, stunBindPort);
            if (!stunRelayUdpAddr.isNull())
                c.ic->setStunRelayUdpService(stunRelayUdpAddr, stunRelayUdpPort, stunRelayUdpUser, stunRelayUdpPass);
            if (!stunRelayTcpAddr.isNull())
                c.ic->setStunRelayTcpService(stunRelayTcpAddr, stunRelayTcpPort, stunRelayTcpUser, stunRelayTcpPass);

            c.ic->setUseLocal(useLocal);
            c.ic->setUseStunBind(useStunBind);
            c.ic->setUseStunRelayUdp(useStunRelayUdp);
            c.ic->setUseStunRelayTcp(useStunRelayTcp);

            // create an inbound queue for this component
            in += QList<QByteArray>();

            c.ic->update(&socketList);
        }

        // socketList should always empty here, but might not be if
        //   the app provided a different address list to
        //   UdpPortReserver and Ice176.  and that would really be
        //   a dumb thing to do but I'm not going to Q_ASSERT it
        if (!socketList.isEmpty())
            portReserver->returnSockets(socketList);
    }

    void stop()
    {
        Q_ASSERT(state == Starting || state == Started || state == Nominating || state == Active);

        state = Stopping;
        remoteGatheringCompleteTimer.reset();

        // will trigger candidateRemoved events and result pairs cleanup.
        if (!components.empty()) {
            for (auto &c : components) {
                c.nominationTimer.reset();
                c.ic->stop();
            }

        } else {
            // TODO: hmm, is it possible to have no components?
            QMetaObject::invokeMethod(this, "postStop", Qt::QueuedConnection);
        }
    }

    void addRemoteCandidates(const QList<Candidate> &list)
    {
        updateRemoteGatheringTimeout();
        QList<IceComponent::CandidateInfo::Ptr> remoteCandidates;
        for (const Candidate &c : list) {
            auto ci       = IceComponent::CandidateInfo::Ptr::create();
            ci->addr.addr = c.ip;
            ci->addr.addr.setScopeId(QString());
            ci->addr.port   = c.port;
            ci->type        = (IceComponent::CandidateType)string_to_candidateType(c.type); // TODO: handle error
            ci->componentId = c.component;
            ci->priority    = c.priority;
            ci->foundation  = c.foundation;
            if (!c.rel_addr.isNull()) {
                ci->base.addr = c.rel_addr;
                ci->base.addr.setScopeId(QString());
                ci->base.port = c.rel_port;
            }
            ci->network = c.network;
            ci->id      = c.id;

            // find remote prflx with same addr. we have to update them instead adding new one. RFC8445 7.3.1.3
            auto it = std::find_if(this->remoteCandidates.begin(), this->remoteCandidates.end(),
                                   [&](IceComponent::CandidateInfo::Ptr rc) {
                                       return ci->addr == rc->addr && ci->componentId == rc->componentId
                                           && rc->type == IceComponent::PeerReflexiveType;
                                   });
            if (it != this->remoteCandidates.end()) {
                (*it)->type = ci->type; // RFC8445 5.1.2.1.  Recommended Formula (peer-reflexive are preferred)
                                        // B.7.  Why Prefer Peer-Reflexive Candidates?
                                        // if srflx == prflx -> set srflx because not secure anyway
                (*it)->foundation = ci->foundation;
                (*it)->base       = ci->base;
                (*it)->network    = ci->network;
                (*it)->id         = ci->id;
                printf("Previously known remote prflx was updated from signalling: %s", qPrintable((*it)->addr));
            } else {
                remoteCandidates += ci;
            }
        }
        this->remoteCandidates += remoteCandidates;

        printf("adding %d remote candidates. total=%d\n", remoteCandidates.count(), this->remoteCandidates.count());
        doPairing(localCandidates, remoteCandidates);
    }

    void setRemoteGatheringComplete()
    {
        remoteGatheringComplete = true;
        remoteGatheringCompleteTimer.reset();

        printf("Got remote gathering complete signal");
        onRemoteGatheringCompleted();
    }

    // returns a pair is pairable or null
    QSharedPointer<CandidatePair> makeCandidatesPair(IceComponent::CandidateInfo::Ptr lc,
                                                     IceComponent::CandidateInfo::Ptr rc)
    {
        if (lc->componentId != rc->componentId)
            return {};

        // don't pair ipv4 with ipv6.  FIXME: is this right?
        if (lc->addr.addr.protocol() != rc->addr.addr.protocol())
            return {};

        // don't relay to localhost.  turnserver
        //   doesn't like it.  i don't know if this
        //   should qualify as a HACK or not.
        //   trying to relay to localhost is pretty
        //   stupid anyway
        if (lc->type == IceComponent::RelayedType && getAddressScope(rc->addr.addr) == 0)
            return {};

        auto pair    = QSharedPointer<CandidatePair>::create();
        pair->local  = lc;
        pair->remote = rc;
        if (pair->local->addr.addr.protocol() == QAbstractSocket::IPv6Protocol
            && isIPv6LinkLocalAddress(pair->local->addr.addr))
            pair->remote->addr.addr.setScopeId(pair->local->addr.addr.scopeId());
        if (mode == Ice176::Initiator)
            pair->priority = calc_pair_priority(lc->priority, rc->priority);
        else
            pair->priority = calc_pair_priority(rc->priority, lc->priority);

        return pair;
    }

    // adds new pairs, sorts, prunes
    void addChecklistPairs(const QList<QSharedPointer<CandidatePair>> &pairs)
    {
        printf("%d pairs\n", pairs.count());

        // combine pairs with existing, and sort
        checkList.pairs += pairs;
        std::sort(checkList.pairs.begin(), checkList.pairs.end(),
                  [&](const QSharedPointer<CandidatePair> &a, const QSharedPointer<CandidatePair> &b) {
                      return a->priority == b->priority ? a->local->componentId < b->local->componentId
                                                        : a->priority > b->priority;
                  });

        // pruning
        for (int n = 0; n < checkList.pairs.count(); ++n) {
            auto &pair = checkList.pairs[n];
            printf("%d, %s\n", pair->local->componentId, qPrintable(*pair));

            for (int i = n - 1; i >= 0; --i) {
                // RFC8445 says to use base only for reflexive. but base is set properly for host and relayed too.
                if (pair->local->componentId == checkList.pairs[i]->local->componentId
                    && pair->local->base == checkList.pairs[i]->local->base
                    && pair->remote->addr == checkList.pairs[i]->remote->addr) {

                    checkList.pairs.removeAt(n);
                    --n; // adjust position
                    break;
                }
            }
        }

        // max pairs is 100 * number of components
        int max_pairs = 100 * components.size();
        while (checkList.pairs.count() > max_pairs)
            checkList.pairs.removeLast();

        printf("%d after pruning\n", checkList.pairs.count());
        for (auto &p : checkList.pairs) {
            printf("%d, %s\n", p->local->componentId, qPrintable(*p));
        }
    }

    QSharedPointer<CandidatePair> selectNextPairToCheck()
    {
        // rfc8445 6.1.4.2.  Performing Connectivity Checks
        QSharedPointer<CandidatePair> pair;
        while (!checkList.triggeredPairs.empty() && !(pair = checkList.triggeredPairs.dequeue().lock()))
            ;

        if (pair) {
            pair->isTriggered = true;
            // according to rfc - check just this one
            printf("next check from triggered list: %s\n", qPrintable(*pair));
            return pair;
        }

        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](const auto &p) mutable {
            if (p->state == PFrozen && !pair)
                pair = p;
            return p->state == PWaiting;
        });
        if (it != checkList.pairs.end()) { // found waiting
            // the list was sorted already by priority and componentId. So first one is Ok
            printf("next check for already waiting: %s\n", qPrintable(**it));
            (*it)->isTriggered = false;
            return *it;
        }

        if (pair) { // now it's frozen highest-priority pair
            pair->isTriggered = false;
            printf("next check for a frozen pair: %s\n", qPrintable(*pair));
        }

        // FIXME real algo should be (but is requires significant refactoring)
        //   1) go over all knows pair foundations over all checklists
        //   2) if for the foundation there is a frozen pair but no (in-progress or waiting)
        //   3)    - do checks on this pair

        return pair;
    }

    void checkPair(QSharedPointer<CandidatePair> pair)
    {
        pair->foundation = pair->local->foundation + pair->remote->foundation;
        pair->state      = PInProgress;

        int at = findLocalCandidate(pair->local->addr.addr, pair->local->addr.port);
        Q_ASSERT(at != -1);

        auto &lc = localCandidates[at];

        Component &c = *findComponent(lc.info->componentId);

        pair->pool = new StunTransactionPool(StunTransaction::Udp, this);
        connect(pair->pool, SIGNAL(outgoingMessage(QByteArray, QHostAddress, int)),
                SLOT(pool_outgoingMessage(QByteArray, QHostAddress, int)));
        // pair->pool->setUsername(peerUser + ':' + localUser);
        // pair->pool->setPassword(peerPass.toUtf8());

        pair->binding = new StunBinding(pair->pool);
        connect(pair->binding, &StunBinding::success, this, [this, wpair = pair.toWeakRef()]() {
            auto pair = wpair.lock();
            if (pair)
                handlePairBindingSuccess(pair);
        });
        connect(pair->binding, &StunBinding::error, this, [this, wpair = pair.toWeakRef()](XMPP::StunBinding::Error e) {
            auto pair = wpair.lock();
            if (pair)
                handlePairBindingError(pair, e);
        });

        int prflx_priority = c.ic->peerReflexivePriority(lc.iceTransport, lc.path);
        pair->binding->setPriority(prflx_priority);

        if (mode == Ice176::Initiator) {
            pair->binding->setIceControlling(0);
            if (localFeatures & AggressiveNomination || pair->finalNomination)
                pair->binding->setUseCandidate(true);
        } else
            pair->binding->setIceControlled(0);

        pair->binding->setShortTermUsername(peerUser + ':' + localUser);
        pair->binding->setShortTermPassword(peerPass);

        pair->binding->start();
    }

    void doPairing(const QList<IceComponent::Candidate> &         localCandidates,
                   const QList<IceComponent::CandidateInfo::Ptr> &remoteCandidates)
    {
        QList<QSharedPointer<CandidatePair>> pairs;
        for (const IceComponent::Candidate &cc : localCandidates) {
            auto lc = cc.info;
            if (lc->type == IceComponent::PeerReflexiveType) {
                printf("not pairing local prflx. %s\n", qPrintable(lc->addr));
                // see RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
                continue;
            }

            for (IceComponent::CandidateInfo::Ptr rc : remoteCandidates) {
                auto pair = makeCandidatesPair(lc, rc);
                if (!pair.isNull())
                    pairs += pair;
            }
        }

        addChecklistPairs(pairs);

        if (!checkTimer.isActive())
            checkTimer.start();
    }

    void write(int componentIndex, const QByteArray &datagram)
    {
        auto it = std::find_if(checkList.validPairs.begin(), checkList.validPairs.end(),
                               [&](const auto &p) { return p->local->componentId - 1 == componentIndex; });
        if (it == checkList.validPairs.end()) {
            qDebug("An attempt to write to an ICE component w/o valid sockets");
            return;
        }

        auto &pair = *it;
        int   at   = findLocalCandidate(pair->local->addr.addr, pair->local->addr.port);
        if (at == -1) // FIXME: assert?
            return;

        IceComponent::Candidate &lc = localCandidates[at];

        int path = lc.path;

        lc.iceTransport->writeDatagram(path, datagram, pair->remote->addr.addr, pair->remote->addr.port);

        // DOR-SR?
        QMetaObject::invokeMethod(q, "datagramsWritten", Qt::QueuedConnection, Q_ARG(int, componentIndex),
                                  Q_ARG(int, 1));
    }

    void flagComponentAsLowOverhead(int componentIndex)
    {
        // FIXME: ok to assume in order?
        Component &c  = components[componentIndex];
        c.lowOverhead = true;

        // FIXME: actually do something
    }

    void cleanupButSelectedPair(int componentId)
    {
        CandidatePair::Ptr selected;

        auto it = std::remove_if(checkList.validPairs.begin(), checkList.validPairs.end(),
                                 [&, keep = true](auto &pair) mutable {
                                     if (pair->local->componentId == componentId) {
                                         if (keep) {
                                             selected = pair;
                                             keep     = false;
                                             return false;
                                         }
                                         return true;
                                     }
                                     return false;
                                 });
        checkList.validPairs.erase(it, checkList.validPairs.end());
        Q_ASSERT(selected.data() != nullptr);

        auto &sa = selected->local->addr;
        auto &t  = localCandidates[findLocalCandidate(sa.addr, sa.port)].iceTransport;
        // cancel active transactions
        for (auto &p : checkList.pairs) {
            if (p->local->componentId == componentId && p->state == PInProgress) {
                p->binding->cancel();
                p->state = PFailed;
            }
        }
        // stop not used transports
        for (auto &c : localCandidates) {
            if (c.info->componentId == componentId && c.iceTransport != t) {
                c.iceTransport->stop();
            }
        }
    }

    bool canHaveMoreRemoteCandidates() const { return remoteGatheringComplete || !(remoteFeatures & Trickle); }

    void tryNominateSelectedPair()
    {
        if (mode != Initiator || state != Started || checkList.validPairs.isEmpty())
            return;
        Q_ASSERT(!checkList.validPairs.first()->isNominated);
        if (checkList.validPairs.first()->local->type == IceComponent::RelayedType) {
            if (!(localGatheringComplete && canHaveMoreRemoteCandidates())) {
                printf("Wiating for gathering complete on both sides before nomination of relayed pair");
                return; // maybe we gonna have a non-relayed pair. RFC8445 anyway allows to send data on any valid.
            }

            // if there is any non-relayed pending pair
            if (std::any_of(checkList.pairs.begin(), checkList.pairs.end(), [](auto &p) {
                    return p->state != PSucceeded && p->state != PFailed && p->local->type != IceComponent::RelayedType;
                })) {
                printf("There are some non-relayed pairs to check before relayed nomination");
                return; // either till checked or remote gathering timeout
            }
        }
        state                 = Nominating;
        auto pair             = checkList.validPairs.first();
        pair->finalNomination = true;
        printf("Nominating valid pair: %s\n", qPrintable(*pair));
        checkList.triggeredPairs.prepend(pair);
    }

    void tryIceFinished()
    {
        if (!std::all_of(components.begin(), components.end(), [](auto &c) { return c.finalSelected; }))
            return;
        if (!readyToSendMedia) {
            readyToSendMedia = true;
            emit q->readyToSendMedia();
        }
        state = Active;
        emit q->iceFinished();
    }

    // execute when new nominated pair
    void tryComponentSuccess(QSharedPointer<CandidatePair> &pair)
    {
        Component &c      = *findComponent(pair->local->componentId);
        pair->isNominated = true;
        if (c.lowOverhead) {
            printf("component is flagged for low overhead.  setting up for %s\n", qPrintable(*pair));
            auto &cc = localCandidates[findLocalCandidate(pair->local->addr.addr, pair->local->addr.port)];
            c.ic->flagPathAsLowOverhead(cc.id, pair->remote->addr.addr, pair->remote->addr.port);
        }

        emit q->componentReady(pair->local->componentId - 1);

        bool agrNom = (mode == Initiator ? localFeatures : remoteFeatures) & AggressiveNomination;
        if (agrNom) {
            if (c.nominationTimer)
                return;
            // nominated candidate may change. start a timer to cease all checks later
            auto timer = new QTimer();
            c.nominationTimer.reset(timer);
            timer->setSingleShot(true);
            timer->setInterval(agressiveNominationTimeout);
            connect(timer, &QTimer::timeout, this, [this, componentId = pair->local->componentId]() {
                if (state != Started && state != Nominating)
                    return; // likely user stopped or it's alreday active
                auto &finalSelected = findComponent(componentId)->finalSelected;
                if (finalSelected)
                    return;
                cleanupButSelectedPair(componentId);
                finalSelected = true;
                tryIceFinished();
            });
            timer->start();
        } else {
            cleanupButSelectedPair(pair->local->componentId);
            c.finalSelected = true;
            tryIceFinished();
        }
    }

    // ice negotiation failed. either initial or on ICE restart
    void tryComponentFailed(int componentId)
    {
        Q_ASSERT(state == Starting);
        if (!(localGatheringComplete && canHaveMoreRemoteCandidates())) {
            return; // if we have something to gather then we still have a chance for success
        }

        if (std::find_if(checkList.pairs.begin(), checkList.pairs.end(),
                         [&](auto const &p) mutable {
                             return p->local->componentId == componentId
                                 && (p->state != CandidatePairState::PSucceeded
                                     && p->state != CandidatePairState::PFailed);
                         })
            != checkList.pairs.end())
            return; // not all finished

        stop();
        emit q->error(ErrorGeneric);
    }

    // nominated - out side=responder. and remote request had USE_CANDIDATE
    void doTriggeredCheck(const IceComponent::Candidate &locCand, IceComponent::CandidateInfo::Ptr remCand,
                          bool nominated)
    {
        // let's figure out if this pair already in the check list
        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(),
                               [&](auto const &p) { return *(p->local) == locCand.info && *(p->remote) == remCand; });

        CandidatePair::Ptr pair = (it == checkList.pairs.end()) ? CandidatePair::Ptr() : *it;
        if (pair) {
            if (pair->state == CandidatePairState::PSucceeded) {
                // Check nominated here?
                printf("Don't do triggered check since pair is already in success state\n");
                if (mode == Responder && !pair->isNominated && nominated) {
                    tryComponentSuccess(pair);
                }
                return; // nothing todo. rfc 8445 7.3.1.4
            }
            pair->isNominated = false;
            if (pair->state == CandidatePairState::PInProgress) {
                if (pair->isTriggered) {
                    printf("Current in-progress check is already triggered. Don't cancel it while have to according to "
                           "RFC8445\n");
                    return;
                }
                pair->binding->cancel();
            }
            if (pair->state == PFailed) {
                // if (state == Stopped) {
                // TODO Stopped? maybe Failed? and we have to notify the outer world
                //}
            }
        } else {
            // RFC8445 7.3.1.4.  Triggered Checks / "If the pair is not already on the checklist"
            pair = makeCandidatesPair(locCand.info, remCand);
            if (pair.isNull()) {
                return;
            }
            addChecklistPairs(QList<CandidatePair::Ptr>() << pair);
        }

        pair->state                   = PWaiting;
        pair->isTriggeredForNominated = nominated;
        checkList.triggeredPairs.enqueue(pair);

        if (!checkTimer.isActive())
            checkTimer.start();
    }

    void onRemoteGatheringCompleted()
    {
        remoteGatheringComplete = true;
        if (!localGatheringComplete || state != Started)
            return;

        if (!checkList.validPairs.isEmpty() && mode == Initiator && !(localFeatures & AggressiveNomination))
            tryNominateSelectedPair();
    }

    void updateRemoteGatheringTimeout()
    {
        if (remoteFeatures & GatheringComplete || !(remoteFeatures & Trickle)) {
            remoteGatheringCompleteTimer.reset();
            printf("Don't use Remote Gatherging Complete timeout\n");
            return;
        } else if (remoteGatheringCompleteTimer) {
            printf("Remote Gatherging Complete was restarted\n");
            remoteGatheringCompleteTimer->start(); // restart
            return;
        }
        remoteGatheringCompleteTimer.reset(new QTimer(this));
        remoteGatheringCompleteTimer->setSingleShot(true);
        remoteGatheringCompleteTimer->setInterval(remoteGatheringCompleteTimeout);
        connect(remoteGatheringCompleteTimer.get(), &QTimer::timeout, this, [this]() {
            remoteGatheringCompleteTimer.release()->deleteLater();
            printf("Timeout waiting for Gathering Complete signal\n");
            onRemoteGatheringCompleted();
        });
        printf("Start Remote Gatherging Complete timer\n");
        remoteGatheringCompleteTimer->start();
    }

private:
    inline decltype(components)::iterator findComponent(const IceComponent *ic)
    {
        return std::find_if(components.begin(), components.end(), [&](auto &c) { return c.ic == ic; });
    }

    inline decltype(components)::iterator findComponent(int id)
    {
        return std::find_if(components.begin(), components.end(), [&](auto &c) { return c.id == id; });
    }

    int findLocalCandidate(const IceTransport *iceTransport, int path, bool hostAndRelayOnly = false) const
    {
        for (int n = 0; n < localCandidates.count(); ++n) {
            const IceComponent::Candidate &cc = localCandidates[n];
            if (cc.iceTransport == iceTransport && cc.path == path
                && (!hostAndRelayOnly || cc.info->type == IceComponent::RelayedType
                    || cc.info->type == IceComponent::HostType))
                return n;
        }

        return -1;
    }

    int findLocalCandidate(const QHostAddress &fromAddr, int fromPort)
    {
        for (int n = 0; n < localCandidates.count(); ++n) {
            const IceComponent::Candidate &cc = localCandidates[n];
            if (cc.info->addr.addr == fromAddr && cc.info->addr.port == fromPort)
                return n;
        }

        return -1;
    }

    static QString candidateType_to_string(IceComponent::CandidateType type)
    {
        QString out;
        switch (type) {
        case IceComponent::HostType:
            out = "host";
            break;
        case IceComponent::PeerReflexiveType:
            out = "prflx";
            break;
        case IceComponent::ServerReflexiveType:
            out = "srflx";
            break;
        case IceComponent::RelayedType:
            out = "relay";
            break;
        default:
            Q_ASSERT(0);
        }
        return out;
    }

    static int string_to_candidateType(const QString &in)
    {
        if (in == "host")
            return IceComponent::HostType;
        else if (in == "prflx")
            return IceComponent::PeerReflexiveType;
        else if (in == "srflx")
            return IceComponent::ServerReflexiveType;
        else if (in == "relay")
            return IceComponent::RelayedType;
        else
            return -1;
    }

    static void toOutCandidate(const IceComponent::Candidate &cc, Ice176::Candidate &out)
    {
        out.component  = cc.info->componentId;
        out.foundation = cc.info->foundation;
        out.generation = 0; // TODO
        out.id         = cc.info->id;
        out.ip         = cc.info->addr.addr;
        out.ip.setScopeId(QString());
        out.network  = cc.info->network;
        out.port     = cc.info->addr.port;
        out.priority = cc.info->priority;
        out.protocol = "udp";
        if (cc.info->type != IceComponent::HostType) {
            out.rel_addr = cc.info->base.addr;
            out.rel_addr.setScopeId(QString());
            out.rel_port = cc.info->base.port;
        } else {
            out.rel_addr = QHostAddress();
            out.rel_port = -1;
        }
        out.rem_addr = QHostAddress();
        out.rem_port = -1;
        out.type     = candidateType_to_string(cc.info->type);
    }

    void dumpCandidatesAndStart()
    {
        QList<Ice176::Candidate> list;
        for (auto const &cc : localCandidates) {
            Ice176::Candidate c;
            toOutCandidate(cc, c);
            list += c;
        }
        if (list.size())
            emit q->localCandidatesReady(list);

        state = Started;
        emit q->started();
        if (mode == Responder)
            doPairing(localCandidates, remoteCandidates);
    }

    QString generateIdForCandidate()
    {
        QString id;
        do {
            id = IceAgent::randomCredential(10);
        } while (std::find_if(localCandidates.begin(), localCandidates.end(),
                              [&id](auto const &c) { return c.info->id == id; })
                 != localCandidates.end());
        return id;
    }

    void handlePairBindingSuccess(CandidatePair::Ptr pair)
    {
        /*
            RFC8445 7.2.5.2.1.  Non-Symmetric Transport Addresses
            tells us addr:port of source->dest of request MUST match with dest<-source of the response,
            and we should mark the pair as failed if doesn't match.
            But StunTransaction already does this for us in its checkActiveAndFrom.
            So it will fail with timeout instead if response comes from a wrong address.
        */

        StunBinding *binding = pair->binding;
        // pair->isValid = true;
        pair->state                  = CandidatePairState::PSucceeded;
        bool isTriggeredForNominated = pair->isTriggeredForNominated;
        bool finalNomination         = pair->finalNomination;

        printf("check success for %s\n", qPrintable(QString(*pair)));

        // RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
        auto mappedAddr = IceComponent::TransportAddress(binding->reflexiveAddress(), binding->reflexivePort());
        if (pair->local->addr != mappedAddr) { // skip "If the valid pair equals the pair that generated the check"

            // so mapped address doesn't match with local candidate sending binding request.
            // gotta find/create one
            auto locIt = std::find_if(localCandidates.begin(), localCandidates.end(), [&](const auto &c) {
                return c.info->base == mappedAddr || c.info->addr == mappedAddr;
            });
            if (locIt == localCandidates.end()) {
                // RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
                // new peer-reflexive local candidate discovered
                findComponent(pair->local->componentId)
                    ->ic->addLocalPeerReflexiveCandidate(mappedAddr, pair->local, binding->priority());
                locIt = std::find_if(localCandidates.begin(), localCandidates.end(),
                                     [&](const auto &c) { return c.info->addr == mappedAddr; }); // just inserted
                Q_ASSERT(locIt != localCandidates.end());
                // local candidate wasn't found, so it wasn't on the checklist  RFC8445 7.2.5.3.1.3
                pair = makeCandidatesPair(locIt->info, pair->remote);
            } else {
                // local candidate found. If it's a part of a pair on checklist, we have to add this pair to valid list,
                // otherwise we have to create a new pair and add it to valid list
                auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](auto const &p) {
                    return p->local->id == locIt->info->id && p->remote->addr == pair->remote->addr;
                });
                if (it == checkList.pairs.constEnd()) {
                    pair = makeCandidatesPair(locIt->info, pair->remote);
                } else {
                    pair = *it;
                    printf("mapped address belongs to another pair on checklist %s\n", qPrintable(QString(*pair)));
                    if (pair->isValid) { // already valid as result of previous checks probably
                        return;
                    }
                }
            }
        }

        if (!pair) {
            qWarning("binding success but failed to build a pair with mapped address %s!", qPrintable(mappedAddr));
            return;
        }

        pair->isValid                 = true;
        pair->state                   = PSucceeded; // what if it was in progress?
        pair->isTriggeredForNominated = isTriggeredForNominated;
        pair->finalNomination         = finalNomination;

        // mark all with same foundation as Waiting to prioritize them
        for (auto &p : checkList.pairs)
            if (p->state == PFrozen && p->foundation == pair->foundation)
                p->state = PWaiting;

        // find position to insert in sorted list of valid pairs
        auto insIt = std::upper_bound(
            checkList.validPairs.begin(), checkList.validPairs.end(), pair, [](auto &item, auto &toins) {
                if (toins->isNominated ^ item->isNominated)
                    return item->isNominated;
                return item->priority == toins->priority
                    ? item->local->componentId < toins->local->componentId
                    : item->priority >= toins->priority; // inverted since we need high priority first
            });
        checkList.validPairs.insert(insIt, pair); // nominated and highest priority first

        if (!readyToSendMedia && localFeatures & NotNominatedData && remoteFeatures & NotNominatedData) {
            // if both follow RFC8445 and allow to send data on any valid pair
            findComponent(pair->local->componentId)->hasValidPairs = true;
            if (std::all_of(components.begin(), components.end(), [](auto &c) { return c.hasValidPairs; })) {
                readyToSendMedia = true;
                emit q->readyToSendMedia();
            }
        }

        if (mode == Ice176::Initiator) {
            if (!binding->useCandidate()) {
                if (!(localFeatures & AggressiveNomination))
                    tryNominateSelectedPair();
                return;
            }
        } else {
            if (!pair->isTriggeredForNominated)
                return;
        }

        // check if component already has nominated pair
        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](auto const &p) {
            return p->local->componentId == pair->local->componentId && p->isNominated;
        });

        if (it != checkList.pairs.end()) {
            printf("component %d already active, not signalling\n", pair->local->componentId);
            return;
        }
        tryComponentSuccess(pair);
    }

    void handlePairBindingError(CandidatePair::Ptr pair, XMPP::StunBinding::Error)
    {
        Q_ASSERT(state != Stopped);
        if (state == Stopping)
            return; // we don't care about late errors

        printf("check failed for %s\n", qPrintable(*pair));
        pair->state = CandidatePairState::PFailed;
        if (pair->isValid) { // RFC8445 7.2.5.3.4.  Updating the Nominated Flag /  about failure
            checkList.validPairs.removeOne(pair);
            pair->isValid = false;
        }
        if (state == Active) {
            return; // TODO hadle keep-alive binding properly
        }

        if (state == Started) {
            // oops, already-started ICE reports errors. keep-alive checks?
            if (pair->isNominated) {
                printf("check failed on nominated candidate. set ICE status to failed");
                stop();
                emit q->error(ErrorDisconnected);
            }
            return;
        }
        if (state == Nominating) {
            if (pair->finalNomination) {
                printf("Failed to nominate selected pair. set ICE status to failed");
                stop();
                emit q->error(ErrorDisconnected);
            }
            return;
        }

        tryComponentFailed(pair->local->componentId);
    }

private slots:
    void postStop()
    {
        state = Stopped;
        emit q->stopped();
    }

    void ic_candidateAdded(const XMPP::IceComponent::Candidate &_cc)
    {
        IceComponent::Candidate cc = _cc;

        cc.info->id = generateIdForCandidate();

        localCandidates += cc;

        printf("C%d: candidate added: %s %s;%d\n", cc.info->componentId,
               qPrintable(candidateType_to_string(cc.info->type)), qPrintable(cc.info->addr.addr.toString()),
               cc.info->addr.port);

        if (!iceTransports.contains(cc.iceTransport)) {
            connect(cc.iceTransport.data(), SIGNAL(readyRead(int)), SLOT(it_readyRead(int)));
            connect(cc.iceTransport.data(), SIGNAL(datagramsWritten(int, int, QHostAddress, int)),
                    SLOT(it_datagramsWritten(int, int, QHostAddress, int)));

            iceTransports += cc.iceTransport;
        }

        if (!localHostGatheringFinished)
            return; // all local IPs will be reported at once

        if (localFeatures & Trickle) {
            QList<Ice176::Candidate> list;

            Ice176::Candidate c;
            toOutCandidate(cc, c);
            list += c;

            emit q->localCandidatesReady(list);
        }
        if (state == Started) {
            doPairing(QList<IceComponent::Candidate>() << cc, remoteCandidates);
        }
    }

    void ic_candidateRemoved(const XMPP::IceComponent::Candidate &cc)
    {
        // TODO
        printf("C%d: candidate removed: %s;%d\n", cc.info->componentId, qPrintable(cc.info->addr.addr.toString()),
               cc.info->addr.port);

        QStringList idList;
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].id == cc.id && localCandidates[n].info->componentId == cc.info->componentId) {
                // FIXME: this is rather ridiculous I think
                idList += localCandidates[n].info->id;

                localCandidates.removeAt(n);
                --n; // adjust position
            }
        }

        bool iceTransportInUse = false;
        for (const IceComponent::Candidate &lc : localCandidates) {
            if (lc.iceTransport == cc.iceTransport) {
                iceTransportInUse = true;
                break;
            }
        }
        if (!iceTransportInUse) {
            cc.iceTransport->disconnect(this);
            iceTransports.remove(cc.iceTransport);
        }

        for (int n = 0; n < checkList.pairs.count(); ++n) {
            if (idList.contains(checkList.pairs[n]->local->id)) {
                StunBinding *        binding = checkList.pairs[n]->binding;
                StunTransactionPool *pool    = checkList.pairs[n]->pool;

                delete binding;

                if (pool) {
                    pool->disconnect(this);
                    pool->setParent(nullptr);
                    pool->deleteLater();
                }

                checkList.pairs.removeAt(n);
                --n; // adjust position
            }
        }
    }

    void ic_localFinished()
    {
        IceComponent *ic = static_cast<IceComponent *>(sender());
        auto          it = findComponent(ic);
        Q_ASSERT(it != components.end());
        Q_ASSERT(!it->localFinished);

        it->localFinished = true;

        for (const Component &c : components) {
            if (!c.localFinished) {
                return;
            }
        }

        localHostGatheringFinished = true;
        if (localFeatures & Trickle)
            dumpCandidatesAndStart();
    }

    void ic_gatheringComplete()
    {
        if (localGatheringComplete)
            return; // wtf? Why are we here then

        for (auto const &c : components) {
            if (!c.ic->isGatheringComplete()) {
                return;
            }
        }
        localGatheringComplete = true;

        if (localFeatures & Trickle) { // It was already started
            emit q->localGatheringComplete();
            return;
        }

        dumpCandidatesAndStart();
    }

    void ic_stopped()
    {
        IceComponent *ic = static_cast<IceComponent *>(sender());
        auto          it = findComponent(ic);
        Q_ASSERT(it != components.end());

        it->stopped = true;

        bool allStopped = true;
        for (const Component &c : components) {
            if (!c.stopped) {
                allStopped = false;
                break;
            }
        }

        if (allStopped)
            postStop();
    }

    void ic_debugLine(const QString &line)
    {
        IceComponent *ic = static_cast<IceComponent *>(sender());
        auto          it = findComponent(ic);
        Q_ASSERT(it != components.end());

        // FIXME: components are always sorted?
        printf("C%d: %s\n", it->id, qPrintable(line));
    }

    // path is either direct or relayed
    void it_readyRead(int path)
    {
        IceTransport *it = static_cast<IceTransport *>(sender());
        int           at = findLocalCandidate(it, path, true); // just host or relay
        Q_ASSERT(at != -1);

        IceComponent::Candidate &locCand = localCandidates[at];

        IceTransport *sock = it;

        while (sock->hasPendingDatagrams(path)) {
            QHostAddress fromAddr;
            int          fromPort;
            QByteArray   buf = sock->readDatagram(path, &fromAddr, &fromPort);

            // printf("port %d: received packet (%d bytes)\n", lt->sock->localPort(), buf.size());

            QString    requser = localUser + ':' + peerUser;
            QByteArray reqkey  = localPass.toUtf8();

            StunMessage::ConvertResult result;
            StunMessage                msg = StunMessage::fromBinary(buf, &result,
                                                      StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
            if (!msg.isNull() && (msg.mclass() == StunMessage::Request || msg.mclass() == StunMessage::Indication)) {
                printf("received validated request or indication from %s:%d\n", qPrintable(fromAddr.toString()),
                       fromPort);
                QString user = QString::fromUtf8(msg.attribute(StunTypes::USERNAME));
                if (requser != user) {
                    printf("user [%s] is wrong.  it should be [%s].  skipping\n", qPrintable(user),
                           qPrintable(requser));
                    continue;
                }

                if (msg.method() != StunTypes::Binding) {
                    printf("not a binding request.  skipping\n");
                    continue;
                }

                StunMessage response;
                response.setClass(StunMessage::SuccessResponse);
                response.setMethod(StunTypes::Binding);
                response.setId(msg.id());

                QList<StunMessage::Attribute> list;
                StunMessage::Attribute        attr;
                attr.type = StunTypes::XOR_MAPPED_ADDRESS;
                attr.value
                    = StunTypes::createXorPeerAddress(fromAddr, quint16(fromPort), response.magic(), response.id());
                list += attr;

                response.setAttributes(list);

                QByteArray packet = response.toBinary(StunMessage::MessageIntegrity | StunMessage::Fingerprint, reqkey);
                sock->writeDatagram(path, packet, fromAddr, fromPort);

                auto it        = std::find_if(remoteCandidates.begin(), remoteCandidates.end(),
                                       [&](IceComponent::CandidateInfo::Ptr remCand) {
                                           return remCand->componentId == locCand.info->componentId
                                               && remCand->addr.addr == fromAddr && remCand->addr.port == fromPort;
                                       });
                bool nominated = false;
                if (mode == Responder)
                    nominated = msg.hasAttribute(StunTypes::USE_CANDIDATE);
                if (it == remoteCandidates.end()) {
                    // RFC8445 7.3.1.3.  Learning Peer-Reflexive Candidates
                    printf("found NEW remote prflx! %s:%d\n", qPrintable(fromAddr.toString()), fromPort);
                    quint32 priority;
                    StunTypes::parsePriority(msg.attribute(StunTypes::PRIORITY), &priority);
                    auto remCand = IceComponent::CandidateInfo::makeRemotePrflx(locCand.info->componentId, fromAddr,
                                                                                fromPort, priority);
                    remoteCandidates += remCand;
                    doTriggeredCheck(locCand, remCand, nominated);
                } else {
                    doTriggeredCheck(locCand, *it, nominated);
                }
            } else {
                QByteArray  reskey = peerPass.toUtf8();
                StunMessage msg    = StunMessage::fromBinary(
                    buf, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, reskey);
                if (!msg.isNull()
                    && (msg.mclass() == StunMessage::SuccessResponse || msg.mclass() == StunMessage::ErrorResponse)) {
                    printf("received validated response from %s:%d to %s\n", qPrintable(fromAddr.toString()), fromPort,
                           qPrintable(locCand.info->addr));

                    // FIXME: this is so gross and completely defeats the point of having pools
                    for (int n = 0; n < checkList.pairs.count(); ++n) {
                        CandidatePair &pair = *checkList.pairs[n];
                        if (pair.state == PInProgress && pair.local->addr.addr == locCand.info->addr.addr
                            && pair.local->addr.port == locCand.info->addr.port)
                            pair.pool->writeIncomingMessage(msg);
                    }
                } else {
                    // printf("received some non-stun or invalid stun packet\n");

                    // FIXME: i don't know if this is good enough
                    if (StunMessage::isProbablyStun(buf)) {
                        printf("unexpected stun packet (loopback?), skipping.\n");
                        continue;
                    }

                    int at = -1;
                    for (int n = 0; n < checkList.pairs.count(); ++n) {
                        CandidatePair &pair = *checkList.pairs[n];
                        if (pair.local->addr.addr == locCand.info->addr.addr
                            && pair.local->addr.port == locCand.info->addr.port) {
                            at = n;
                            break;
                        }
                    }
                    if (at == -1) {
                        printf("the local transport does not seem to be associated with a candidate?!\n");
                        continue;
                    }

                    int componentIndex = checkList.pairs[at]->local->componentId - 1;
                    // printf("packet is considered to be application data for component index %d\n", componentIndex);

                    // FIXME: this assumes components are ordered by id in our local arrays
                    in[componentIndex] += buf;
                    emit q->readyRead(componentIndex);
                }
            }
        }
    }

    void it_datagramsWritten(int path, int count, const QHostAddress &addr, int port)
    {
        // TODO
        Q_UNUSED(path);
        Q_UNUSED(count);
        Q_UNUSED(addr);
        Q_UNUSED(port);
    }

    void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &addr, int port)
    {
        Q_UNUSED(addr);
        Q_UNUSED(port);

        StunTransactionPool *pool = static_cast<StunTransactionPool *>(sender());
        int                  at   = -1;
        for (int n = 0; n < checkList.pairs.count(); ++n) {
            if (checkList.pairs[n]->pool == pool) {
                at = n;
                break;
            }
        }
        if (at == -1) // FIXME: assert?
            return;

        CandidatePair &pair = *checkList.pairs[at];

        at = findLocalCandidate(pair.local->addr.addr, pair.local->addr.port);
        if (at == -1) // FIXME: assert?
            return;

        IceComponent::Candidate &lc = localCandidates[at];

        int path = lc.path;

        printf("connectivity check for pair %s%s\n", qPrintable(pair),
               (mode == Initiator ? (pair.binding->useCandidate() ? " (nominating)" : "")
                                  : (pair.isTriggeredForNominated ? " (triggered check for nominated)" : "")));
        lc.iceTransport->writeDatagram(path, packet, pair.remote->addr.addr, pair.remote->addr.port);
    }
};

Ice176::Ice176(QObject *parent) : QObject(parent) { d = new Private(this); }

Ice176::~Ice176() { delete d; }

void Ice176::reset() { d->reset(); }

void Ice176::setProxy(const TurnClient::Proxy &proxy) { d->proxy = proxy; }

void Ice176::setPortReserver(UdpPortReserver *portReserver)
{
    Q_ASSERT(d->state == Private::Stopped);

    d->portReserver = portReserver;
}

void Ice176::setLocalAddresses(const QList<LocalAddress> &addrs) { d->updateLocalAddresses(addrs); }

void Ice176::setExternalAddresses(const QList<ExternalAddress> &addrs) { d->updateExternalAddresses(addrs); }

void Ice176::setStunBindService(const QHostAddress &addr, int port)
{
    d->stunBindAddr = addr;
    d->stunBindPort = port;
}

void Ice176::setStunRelayUdpService(const QHostAddress &addr, int port, const QString &user,
                                    const QCA::SecureArray &pass)
{
    d->stunRelayUdpAddr = addr;
    d->stunRelayUdpPort = port;
    d->stunRelayUdpUser = user;
    d->stunRelayUdpPass = pass;
}

void Ice176::setStunRelayTcpService(const QHostAddress &addr, int port, const QString &user,
                                    const QCA::SecureArray &pass)
{
    d->stunRelayTcpAddr = addr;
    d->stunRelayTcpPort = port;
    d->stunRelayTcpUser = user;
    d->stunRelayTcpPass = pass;
}

void Ice176::setUseLocal(bool enabled) { d->useLocal = enabled; }

void Ice176::setUseStunBind(bool enabled) { d->useStunBind = enabled; }

void Ice176::setUseStunRelayUdp(bool enabled) { d->useStunRelayUdp = enabled; }

void Ice176::setUseStunRelayTcp(bool enabled) { d->useStunRelayTcp = enabled; }

void Ice176::setComponentCount(int count)
{
    Q_ASSERT(d->state == Private::Stopped);

    d->componentCount = count;
}

void Ice176::setLocalFeatures(const Features &features) { d->localFeatures = features; }

void Ice176::setRemoteFeatures(const Features &features)
{
    d->remoteFeatures = features;
    d->updateRemoteGatheringTimeout();
}

void Ice176::start(Mode mode)
{
    d->mode = mode;
    d->start();
}

void Ice176::stop() { d->stop(); }

QString Ice176::localUfrag() const { return d->localUser; }

QString Ice176::localPassword() const { return d->localPass; }

void Ice176::setPeerUfrag(const QString &ufrag) { d->peerUser = ufrag; }

void Ice176::setPeerPassword(const QString &pass) { d->peerPass = pass; }

void Ice176::addRemoteCandidates(const QList<Candidate> &list) { d->addRemoteCandidates(list); }

void Ice176::setRemoteGatheringComplete() { d->setRemoteGatheringComplete(); }

bool Ice176::canSendMedia() const { return d->readyToSendMedia; }

bool Ice176::hasPendingDatagrams(int componentIndex) const { return !d->in[componentIndex].isEmpty(); }

QByteArray Ice176::readDatagram(int componentIndex) { return d->in[componentIndex].takeFirst(); }

void Ice176::writeDatagram(int componentIndex, const QByteArray &datagram) { d->write(componentIndex, datagram); }

void Ice176::flagComponentAsLowOverhead(int componentIndex) { d->flagComponentAsLowOverhead(componentIndex); }

bool Ice176::isIPv6LinkLocalAddress(const QHostAddress &addr)
{
    Q_ASSERT(addr.protocol() == QAbstractSocket::IPv6Protocol);
    Q_IPV6ADDR addr6 = addr.toIPv6Address();
    quint16    hi    = addr6[0];
    hi <<= 8;
    hi += addr6[1];
    if ((hi & 0xffc0) == 0xfe80)
        return true;
    else
        return false;
}

} // namespace XMPP

#include "ice176.moc"
