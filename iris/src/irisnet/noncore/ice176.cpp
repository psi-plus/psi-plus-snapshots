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

// see if candidates are considered the same for pruning purposes
static bool compare_candidates(const IceComponent::CandidateInfo &a, const IceComponent::CandidateInfo &b)
{
    if (a.addr == b.addr && a.componentId == b.componentId)
        return true;
    else
        return false;
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
    enum State { Stopped, Starting, Started, Nominating, Stopping };

    enum CandidatePairState { PWaiting, PInProgress, PSucceeded, PFailed, PFrozen };

    enum CheckListState { LRunning, LCompleted, LFailed };

    class CandidatePair {
    public:
        IceComponent::CandidateInfo local, remote;
        bool                        isDefault               = false; // not used in xmpp
        bool                        isValid                 = false; // a pair which is also in valid list
        bool                        isNominated             = false;
        bool                        isTriggeredForNominated = false;
        CandidatePairState          state                   = CandidatePairState::PFrozen;

        qint64  priority = 0;
        QString foundation; // rfc8445 6.1.2.6 (combination of foundations)

        StunBinding *binding = nullptr;

        // FIXME: this is wrong i think, it should be in LocalTransport
        //   or such, to multiplex ids
        StunTransactionPool *pool = nullptr;

        inline bool isNull() const { return local.addr.addr.isNull() || remote.addr.addr.isNull(); }
        inline      operator QString() const
        {
            if (isNull())
                return QLatin1String("null pair");
            return QString(QLatin1String("%1 %2 -> %3 %4"))
                .arg(candidateType_to_string(local.type), QString(local.addr), candidateType_to_string(remote.type),
                     QString(remote.addr));
        }
    };

    class CheckList {
    public:
        QList<QSharedPointer<CandidatePair>> pairs;
        QQueue<QWeakPointer<CandidatePair>>  triggeredPairs;
        QList<QSharedPointer<CandidatePair>> validPairs;
        CheckListState                       state;
    };

    class Component {
    public:
        int           id            = 0;
        IceComponent *ic            = nullptr;
        bool          localFinished = false;
        bool          stopped       = false;
        bool          lowOverhead   = false;
    };

    Ice176 *                           q;
    Ice176::Mode                       mode;
    State                              state = Stopped;
    QTimer                             checkTimer;
    TurnClient::Proxy                  proxy;
    UdpPortReserver *                  portReserver   = nullptr;
    int                                componentCount = 0;
    QList<Ice176::LocalAddress>        localAddrs;
    QList<Ice176::ExternalAddress>     extAddrs;
    QHostAddress                       stunBindAddr;
    int                                stunBindPort;
    QHostAddress                       stunRelayUdpAddr;
    int                                stunRelayUdpPort;
    QString                            stunRelayUdpUser;
    QCA::SecureArray                   stunRelayUdpPass;
    QHostAddress                       stunRelayTcpAddr;
    int                                stunRelayTcpPort;
    QString                            stunRelayTcpUser;
    QCA::SecureArray                   stunRelayTcpPass;
    QString                            localUser, localPass;
    QString                            peerUser, peerPass;
    QList<Component>                   components;
    QList<IceComponent::Candidate>     localCandidates;
    QList<IceComponent::CandidateInfo> remoteCandidates;
    QSet<QWeakPointer<IceTransport>>   iceTransports;
    CheckList                          checkList;
    QList<QList<QByteArray>>           in;
    Features                           remoteFeatures;
    Features                           localFeatures;
    bool                               useLocal                   = true;
    bool                               useStunBind                = true;
    bool                               useStunRelayUdp            = true;
    bool                               useStunRelayTcp            = true;
    bool                               localHostGatheringFinished = false;
    bool                               localGatheringComplete     = false;
    bool                               remoteGatheringComplete    = false;

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
        foreach (const Component &c, components)
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
        foreach (const ExternalAddress &ea, addrs) {
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

        QList<QUdpSocket *> socketList;
        if (portReserver)
            // list size = componentCount * number of interfaces
            socketList = portReserver->borrowSockets(componentCount, this);

        for (int n = 0; n < componentCount; ++n) {
            Component c;
            c.id = n + 1;
            c.ic = new IceComponent(c.id, this);
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

            components += c;

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
        Q_ASSERT(state == Starting || state == Started);

        state = Stopping;

        // will trigger candidateRemoved events and result pairs cleanup.
        if (!components.isEmpty()) {
            for (int n = 0; n < components.count(); ++n)
                components[n].ic->stop();

        } else {
            // TODO: hmm, is it possible to have no components?
            QMetaObject::invokeMethod(this, "postStop", Qt::QueuedConnection);
        }
    }

    void addRemoteCandidates(const QList<Candidate> &list)
    {
        QList<IceComponent::CandidateInfo> remoteCandidates;
        for (const Candidate &c : list) {
            IceComponent::CandidateInfo ci;
            ci.addr.addr = c.ip;
            ci.addr.addr.setScopeId(QString());
            ci.addr.port   = c.port;
            ci.type        = (IceComponent::CandidateType)string_to_candidateType(c.type); // TODO: handle error
            ci.componentId = c.component;
            ci.priority    = c.priority;
            ci.foundation  = c.foundation;
            if (!c.rel_addr.isNull()) {
                ci.base.addr = c.rel_addr;
                ci.base.addr.setScopeId(QString());
                ci.base.port = c.rel_port;
            }
            ci.network = c.network;
            ci.id      = c.id;

            // find remote prflx with same addr. we have to replace them instead adding new one. RFC8445 7.3.1.3
            auto it = std::find_if(this->remoteCandidates.begin(), this->remoteCandidates.end(),
                                   [&](const IceComponent::CandidateInfo &rc) {
                                       return ci.addr.addr == rc.addr.addr && ci.addr.port == rc.addr.port
                                           && ci.componentId == rc.componentId
                                           && rc.type == IceComponent::PeerReflexiveType;
                                   });
            if (it != this->remoteCandidates.end()) {
                it->type       = ci.type;
                it->foundation = ci.foundation;
                it->base       = ci.base;
                it->network    = ci.network;
                it->id         = ci.id;
                printf("Previously known remote prflx was updated from signalling: %s:%d",
                       qPrintable(it->addr.addr.toString()), it->addr.port);
            } else {
                remoteCandidates += ci;
            }
        }
        this->remoteCandidates += remoteCandidates;

        printf("adding %d remote candidates. total=%d\n", remoteCandidates.count(), this->remoteCandidates.count());
        doPairing(localCandidates, remoteCandidates);
    }

    // returns a pair is pairable or null
    QSharedPointer<CandidatePair> makeCandidatesPair(const IceComponent::CandidateInfo &lc,
                                                     const IceComponent::CandidateInfo &rc)
    {
        if (lc.componentId != rc.componentId)
            return {};

        // don't pair ipv4 with ipv6.  FIXME: is this right?
        if (lc.addr.addr.protocol() != rc.addr.addr.protocol())
            return {};

        // don't relay to localhost.  turnserver
        //   doesn't like it.  i don't know if this
        //   should qualify as a HACK or not.
        //   trying to relay to localhost is pretty
        //   stupid anyway
        if (lc.type == IceComponent::RelayedType && getAddressScope(rc.addr.addr) == 0)
            return {};

        auto pair    = QSharedPointer<CandidatePair>::create();
        pair->local  = lc;
        pair->remote = rc;
        if (pair->local.addr.addr.protocol() == QAbstractSocket::IPv6Protocol
            && isIPv6LinkLocalAddress(pair->local.addr.addr))
            pair->remote.addr.addr.setScopeId(pair->local.addr.addr.scopeId());
        if (mode == Ice176::Initiator)
            pair->priority = calc_pair_priority(lc.priority, rc.priority);
        else
            pair->priority = calc_pair_priority(rc.priority, lc.priority);

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
                      return a->priority == b->priority ? a->local.componentId < b->local.componentId
                                                        : a->priority > b->priority;
                  });

        // pruning
        for (auto &pair : checkList.pairs) {
            if (pair->local.type == IceComponent::ServerReflexiveType)
                pair->local.addr = pair->local.base;
        }

        for (int n = 0; n < checkList.pairs.count(); ++n) {
            auto &pair = checkList.pairs[n];
            printf("%d, %s -> %s\n", pair->local.componentId, qPrintable(pair->local.addr),
                   qPrintable(pair->remote.addr));

            bool found = false;
            for (int i = n - 1; i >= 0; --i) {
                if (compare_candidates(pair->local, checkList.pairs[i]->local)
                    && compare_candidates(pair->remote, checkList.pairs[i]->remote)) {
                    found = true;
                    break;
                }
            }

            if (found) {
                checkList.pairs.removeAt(n);
                --n; // adjust position
            }
        }

        // max pairs is 100 * number of components
        int max_pairs = 100 * components.count();
        while (checkList.pairs.count() > max_pairs)
            checkList.pairs.removeLast();

        printf("%d after pruning\n", checkList.pairs.count());
    }

    QSharedPointer<CandidatePair> selectNextPairToCheck()
    {
        // rfc8445 6.1.4.2.  Performing Connectivity Checks

        QSharedPointer<CandidatePair> pair;
        while (!checkList.triggeredPairs.empty() && !(pair = checkList.triggeredPairs.dequeue().lock()))
            ;

        if (pair) {
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
            return *it;
        }

        if (pair) { // now it's frozen highest-priority pair
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
        pair->foundation = pair->local.foundation + pair->remote.foundation;
        pair->state      = PInProgress;

        int at = findLocalCandidate(pair->local.addr.addr, pair->local.addr.port);
        Q_ASSERT(at != -1);

        auto &lc = localCandidates[at];

        Component &c = components[findComponent(lc.info.componentId)];

        pair->pool = new StunTransactionPool(StunTransaction::Udp, this);
        connect(pair->pool, SIGNAL(outgoingMessage(QByteArray, QHostAddress, int)),
                SLOT(pool_outgoingMessage(QByteArray, QHostAddress, int)));
        // pair->pool->setUsername(peerUser + ':' + localUser);
        // pair->pool->setPassword(peerPass.toUtf8());

        pair->binding = new StunBinding(pair->pool);
        connect(pair->binding, SIGNAL(success()), SLOT(binding_success()));
        connect(pair->binding, &StunBinding::error, this, &Ice176::Private::binding_error);

        int prflx_priority = c.ic->peerReflexivePriority(lc.iceTransport, lc.path);
        pair->binding->setPriority(prflx_priority);

        if (mode == Ice176::Initiator) {
            pair->binding->setIceControlling(0);
            if (localFeatures & AggressiveNomination)
                pair->binding->setUseCandidate(true);
        } else
            pair->binding->setIceControlled(0);

        pair->binding->setShortTermUsername(peerUser + ':' + localUser);
        pair->binding->setShortTermPassword(peerPass);

        pair->binding->start();
    }

    void doPairing(const QList<IceComponent::Candidate> &    localCandidates,
                   const QList<IceComponent::CandidateInfo> &remoteCandidates)
    {
        QList<QSharedPointer<CandidatePair>> pairs;
        for (const IceComponent::Candidate &cc : localCandidates) {
            const IceComponent::CandidateInfo &lc = cc.info;
            if (lc.type == IceComponent::PeerReflexiveType) {
                printf("not pairing local prflx. %s\n", qPrintable(lc.addr));
                // see RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
                continue;
            }

            for (const IceComponent::CandidateInfo &rc : remoteCandidates) {
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
                               [&](const auto &p) { return p->local.componentId - 1 == componentIndex; });
        if (it == checkList.validPairs.end()) {
            qDebug("An attempt to write to an ICE component w/o valid sockets");
            return;
        }

        auto &pair = *it;
        int   at   = findLocalCandidate(pair->local.addr.addr, pair->local.addr.port);
        if (at == -1) // FIXME: assert?
            return;

        IceComponent::Candidate &lc = localCandidates[at];

        int path = lc.path;

        lc.iceTransport->writeDatagram(path, datagram, pair->remote.addr.addr, pair->remote.addr.port);

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

    void tryComponentSuccess(QSharedPointer<CandidatePair> &pair)
    {
        // TODO the pair can change with aggressive validation (?)
        pair->isNominated = true;

        int        at = findComponent(pair->local.componentId);
        Component &c  = components[at];
        if (c.lowOverhead) {
            printf("component is flagged for low overhead.  setting up for %s\n", qPrintable(*pair));
            at                          = findLocalCandidate(pair->local.addr.addr, pair->local.addr.port);
            IceComponent::Candidate &cc = localCandidates[at];
            c.ic->flagPathAsLowOverhead(cc.id, pair->remote.addr.addr, pair->remote.addr.port);
        }

        emit q->componentReady(pair->local.componentId - 1);
    }

    // ice negotiation failed. either initial or on ICE restart
    void tryComponentFailed(int componentId)
    {
        Q_ASSERT(state == Starting);
        if (!(localGatheringComplete && remoteGatheringComplete)) {
            return; // if we have something to gather then we still have a chance for success
        }

        if (std::find_if(checkList.pairs.begin(), checkList.pairs.end(),
                         [&](auto const &p) mutable {
                             return p->local.componentId == componentId
                                 && (p->state != CandidatePairState::PSucceeded
                                     && p->state != CandidatePairState::PFailed);
                         })
            != checkList.pairs.end())
            return; // not all finished

        stop();
        emit q->error(ErrorGeneric);
    }

    // nominated - out side=responder. and remote request had USE_CANDIDATE
    void doTriggeredCheck(const IceComponent::Candidate &locCand, const IceComponent::CandidateInfo &remCand,
                          bool nominated)
    {
        // let's figure out if this pair already in the check list
        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](auto const &p) {
            return p->local.isSame(locCand.info) && p->remote.isSame(remCand);
        });
        QSharedPointer<CandidatePair> pair = (it == checkList.pairs.end()) ? QSharedPointer<CandidatePair>() : *it;
        if (pair) {
            if (pair->state == CandidatePairState::PSucceeded) {
                // Check nominated here?
                printf("Don't do triggered check since pair is already in success state\n");
                if (mode == Responder && !pair->isNominated && nominated) {
                    pair->isNominated = true;
                    tryComponentSuccess(pair);
                }
                return; // nothing todo. rfc 8445 7.3.1.4
            }
            pair->isNominated = false;
            if (pair->state == CandidatePairState::PInProgress) {
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
            addChecklistPairs(QList<QSharedPointer<CandidatePair>>() << pair);
        }

        pair->state                   = PWaiting;
        pair->isTriggeredForNominated = nominated;
        checkList.triggeredPairs.enqueue(pair);

        if (!checkTimer.isActive())
            checkTimer.start();
    }

private:
    int findComponent(const IceComponent *ic) const
    {
        for (int n = 0; n < components.count(); ++n) {
            if (components[n].ic == ic)
                return n;
        }

        return -1;
    }

    int findComponent(int id) const
    {
        for (int n = 0; n < components.count(); ++n) {
            if (components[n].id == id)
                return n;
        }

        return -1;
    }

    int findLocalCandidate(const IceTransport *iceTransport, int path, bool withSrvRflx = true) const
    {
        for (int n = 0; n < localCandidates.count(); ++n) {
            const IceComponent::Candidate &cc = localCandidates[n];
            if (cc.iceTransport == iceTransport && cc.path == path
                && (withSrvRflx || cc.info.type != IceComponent::ServerReflexiveType))
                return n;
        }

        return -1;
    }

    int findLocalCandidate(const QHostAddress &fromAddr, int fromPort)
    {
        for (int n = 0; n < localCandidates.count(); ++n) {
            const IceComponent::Candidate &cc = localCandidates[n];
            if (cc.info.addr.addr == fromAddr && cc.info.addr.port == fromPort)
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

    static int compare_pair(const CandidatePair &a, const CandidatePair &b)
    {
        // prefer remote srflx, for leap
        if (a.remote.type == IceComponent::ServerReflexiveType && b.remote.type != IceComponent::ServerReflexiveType
            && b.remote.addr.addr.protocol() != QAbstractSocket::IPv6Protocol)
            return -1;
        else if (b.remote.type == IceComponent::ServerReflexiveType
                 && a.remote.type != IceComponent::ServerReflexiveType
                 && a.remote.addr.addr.protocol() != QAbstractSocket::IPv6Protocol)
            return 1;

        if (a.priority > b.priority)
            return -1;
        else if (b.priority > a.priority)
            return 1;

        return 0;
    }

    static void toOutCandidate(const IceComponent::Candidate &cc, Ice176::Candidate &out)
    {
        out.component  = cc.info.componentId;
        out.foundation = cc.info.foundation;
        out.generation = 0; // TODO
        out.id         = cc.info.id;
        out.ip         = cc.info.addr.addr;
        out.ip.setScopeId(QString());
        out.network  = cc.info.network;
        out.port     = cc.info.addr.port;
        out.priority = cc.info.priority;
        out.protocol = "udp";
        if (cc.info.type != IceComponent::HostType) {
            out.rel_addr = cc.info.base.addr;
            out.rel_addr.setScopeId(QString());
            out.rel_port = cc.info.base.port;
        } else {
            out.rel_addr = QHostAddress();
            out.rel_port = -1;
        }
        out.rem_addr = QHostAddress();
        out.rem_port = -1;
        out.type     = candidateType_to_string(cc.info.type);
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
                              [&id](auto const &c) { return c.info.id == id; })
                 != localCandidates.end());
        return id;
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

        cc.info.id = generateIdForCandidate();

        localCandidates += cc;

        printf("C%d: candidate added: %s %s;%d\n", cc.info.componentId,
               qPrintable(candidateType_to_string(cc.info.type)), qPrintable(cc.info.addr.addr.toString()),
               cc.info.addr.port);

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
        printf("C%d: candidate removed: %s;%d\n", cc.info.componentId, qPrintable(cc.info.addr.addr.toString()),
               cc.info.addr.port);

        QStringList idList;
        for (int n = 0; n < localCandidates.count(); ++n) {
            if (localCandidates[n].id == cc.id && localCandidates[n].info.componentId == cc.info.componentId) {
                // FIXME: this is rather ridiculous I think
                idList += localCandidates[n].info.id;

                localCandidates.removeAt(n);
                --n; // adjust position
            }
        }

        bool iceTransportInUse = false;
        foreach (const IceComponent::Candidate &lc, localCandidates) {
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
            if (idList.contains(checkList.pairs[n]->local.id)) {
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
        int           at = findComponent(ic);
        Q_ASSERT(at != -1);
        Q_ASSERT(!components[at].localFinished);

        components[at].localFinished = true;

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
        int           at = findComponent(ic);
        Q_ASSERT(at != -1);

        components[at].stopped = true;

        bool allStopped = true;
        foreach (const Component &c, components) {
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
        int           at = findComponent(ic);
        Q_ASSERT(at != -1);

        // FIXME: components are always sorted?
        printf("C%d: %s\n", at + 1, qPrintable(line));
    }

    // path is either direct or relayed
    void it_readyRead(int path)
    {
        IceTransport *it = static_cast<IceTransport *>(sender());
        int           at = findLocalCandidate(it, path, false); // without server-reflexive
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
                QString user = QString::fromUtf8(msg.attribute(0x0006)); // USERNAME
                if (requser != user) {
                    printf("user [%s] is wrong.  it should be [%s].  skipping\n", qPrintable(user),
                           qPrintable(requser));
                    continue;
                }

                if (msg.method() != 0x001) {
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
                                       [&](const IceComponent::CandidateInfo &remCand) {
                                           return remCand.componentId == locCand.info.componentId
                                               && remCand.addr.addr == fromAddr && remCand.addr.port == fromPort;
                                       });
                bool nominated = false;
                if (mode == Responder)
                    nominated = msg.hasAttribute(StunTypes::USE_CANDIDATE);
                if (it == remoteCandidates.end()) {
                    printf("found NEW remote prflx! %s:%d\n", qPrintable(fromAddr.toString()), fromPort);
                    quint32 priority;
                    StunTypes::parsePriority(msg.attribute(StunTypes::PRIORITY), &priority);
                    auto remCand = IceComponent::CandidateInfo::makeRemotePrflx(locCand.info.componentId, fromAddr,
                                                                                fromPort, priority);
                    // remoteCandidates += remCand; // RFC8445 7.2.5.3.2.3 hints we shouldn't do that
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
                           qPrintable(locCand.info.addr));

                    // FIXME: this is so gross and completely defeats the point of having pools
                    for (int n = 0; n < checkList.pairs.count(); ++n) {
                        CandidatePair &pair = *checkList.pairs[n];
                        if (pair.state == PInProgress && pair.local.addr.addr == locCand.info.addr.addr
                            && pair.local.addr.port == locCand.info.addr.port)
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
                        if (pair.local.addr.addr == locCand.info.addr.addr
                            && pair.local.addr.port == locCand.info.addr.port) {
                            at = n;
                            break;
                        }
                    }
                    if (at == -1) {
                        printf("the local transport does not seem to be associated with a candidate?!\n");
                        continue;
                    }

                    int componentIndex = checkList.pairs[at]->local.componentId - 1;
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

        at = findLocalCandidate(pair.local.addr.addr, pair.local.addr.port);
        if (at == -1) // FIXME: assert?
            return;

        IceComponent::Candidate &lc = localCandidates[at];

        int path = lc.path;

        printf("connectivity check from %s:%d to %s:%d %s\n", qPrintable(pair.local.addr.addr.toString()),
               pair.local.addr.port, qPrintable(pair.remote.addr.addr.toString()), pair.remote.addr.port,
               pair.isNominated ? (mode == Initiator ? "(nominating)" : "(triggered check for nominated)") : "");
        lc.iceTransport->writeDatagram(path, packet, pair.remote.addr.addr, pair.remote.addr.port);
    }

    void binding_success()
    {
        /*
            RFC8445 7.2.5.2.1.  Non-Symmetric Transport Addresses
            tells us addr:port of source->dest of request MUST match with dest<-source of the response,
            and we should mark the pair as failed if doesn't match.
            But StunTransaction already does this for us in its checkActiveAndFrom.
            So it will fail with timeout instead if response comes from a wrong address.
        */

        StunBinding *binding = static_cast<StunBinding *>(sender());

        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(),
                               [&](auto const &p) { return p->binding == binding; });
        if (it == checkList.pairs.constEnd())
            return;

        auto pair = *it;
        // pair->isValid = true;
        pair->state = CandidatePairState::PSucceeded;

        printf("check success for %s\n", qPrintable(QString(*pair)));

        // RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
        auto mappedAddr = IceComponent::TransportAddress(binding->reflexiveAddress(), binding->reflexivePort());
        if (pair->local.addr != mappedAddr) { // skip "If the valid pair equals the pair that generated the check"

            // so mapped address doesn't match with local candidate sending binding request.
            // gotta find/create one
            auto locIt = std::find_if(localCandidates.begin(), localCandidates.end(), [&](const auto &c) {
                return c.info.base == mappedAddr || c.info.addr == mappedAddr;
            });
            if (locIt == localCandidates.end()) {
                // RFC8445 7.2.5.3.1.  Discovering Peer-Reflexive Candidates
                // new peer-reflexive local candidate discovered
                components[findComponent(pair->local.componentId)].ic->addLocalPeerReflexiveCandidate(
                    mappedAddr, pair->local, binding->priority());
                locIt = std::find_if(localCandidates.begin(), localCandidates.end(),
                                     [&](const auto &c) { return c.info.addr == mappedAddr; }); // just inserted
                Q_ASSERT(locIt != localCandidates.end());
                // local candidate wasn't found, so it wasn't on the checklist  RFC8445 7.2.5.3.1.3
                pair = makeCandidatesPair(locIt->info, pair->remote);
                // TODO start media flow on valid pair
            } else {
                // local candidate found. If it's a part of a pair on checklist, we have to add this pair to valid list,
                // otherwise we have to create a new pair and add it to valid list
                it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](auto const &p) {
                    return p->local.id == locIt->info.id && p->remote.addr == pair->remote.addr;
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

        pair->isValid = true;
        pair->state   = PSucceeded; // what if it was in progress?
        checkList.validPairs.append(pair);

        if (mode == Ice176::Initiator) {
            if (!binding->useCandidate())
                return;
        } else {
            if (!pair->isTriggeredForNominated)
                return;
        }

        // check if component already has nominated pair
        it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(), [&](auto const &p) {
            return p->local.componentId == pair->local.componentId && p->isNominated;
        });

        if (it != checkList.pairs.end()) {
            printf("component %d already active, not signalling\n", pair->local.componentId);
            return;
        }
        tryComponentSuccess(pair);
    }

    void binding_error(XMPP::StunBinding::Error)
    {
        Q_ASSERT(state != Stopped);
        if (state == Stopping)
            return; // we don't care about late errors

        StunBinding *binding = static_cast<StunBinding *>(sender());

        auto it = std::find_if(checkList.pairs.begin(), checkList.pairs.end(),
                               [&](auto const &p) { return p->binding == binding; });
        if (it == checkList.pairs.constEnd())
            return;

        printf("check failed\n");

        CandidatePair &pair = **it;
        pair.state          = CandidatePairState::PFailed;

        if (state == Started) {
            // oops already started ICE reports errors. keep-alive checks?
            if (pair.isNominated) {
                printf("check failed on nominated candidate. set ICE status to failed");
                stop();
                emit q->error(ErrorDisconnected);
            }
            return;
        }

        tryComponentFailed(pair.local.componentId);
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

void Ice176::setRemoteFeatures(const Features &features) { d->remoteFeatures = features; }

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

void Ice176::setRemoteGatheringComplete() { d->remoteGatheringComplete = true; }

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
