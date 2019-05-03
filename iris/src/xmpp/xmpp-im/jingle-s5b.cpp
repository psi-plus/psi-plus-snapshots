/*
 * jignle-s5b.cpp - Jingle SOCKS5 transport
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

#include "jingle-s5b.h"
#include "s5b.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_serverinfomanager.h"

#include <QTimer>

namespace XMPP {
namespace Jingle {
namespace S5B {

const QString NS(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));

class Candidate::Private : public QSharedData {
public:
    QString cid;
    QString host;
    Jid jid;
    quint16 port;
    quint32 priority;
    Candidate::Type type = Candidate::Direct;
    Candidate::State state = Candidate::New;
};

Candidate::Candidate()
{

}

Candidate::Candidate(const QDomElement &el)
{
    bool ok;
    QString host(el.attribute(QStringLiteral("host")));
    Jid jid(el.attribute(QStringLiteral("jid")));
    auto portStr = el.attribute(QStringLiteral("port"));
    quint16 port = 0;
    if (!portStr.isEmpty()) {
        port = portStr.toUShort(&ok);
        if (!ok) {
            return; // make the whole candidate invalid
        }
    }
    auto priorityStr = el.attribute(QStringLiteral("priority"));
    if (priorityStr.isEmpty()) {
        return;
    }
    quint32 priority = priorityStr.toUInt(&ok);
    if (!ok) {
        return; // make the whole candidate invalid
    }
    QString cid = el.attribute(QStringLiteral("cid"));
    if (cid.isEmpty()) {
        return;
    }

    QString ct = el.attribute(QStringLiteral("type"));
    if (ct.isEmpty()) {
        ct = QStringLiteral("direct");
    }
    static QMap<QString,Type> types{{QStringLiteral("assisted"), Assisted},
                                    {QStringLiteral("direct"),   Direct},
                                    {QStringLiteral("proxy"),    Proxy},
                                    {QStringLiteral("tunnel"),   Tunnel}
                                   };
    auto candidateType = types.value(ct);
    if (ct.isEmpty() || candidateType == None) {
        return;
    }

    if ((candidateType == Proxy && !jid.isValid()) ||
            (candidateType != Proxy && (host.isEmpty() || !port)))
    {
        return;
    }

    auto d = new Private;
    d->cid = cid;
    d->host = host;
    d->jid = jid;
    d->port = port;
    d->priority = priority;
    d->type = candidateType;
    d->state = New;
    this->d = d;
}

Candidate::Candidate(const Candidate &other) :
    d(other.d)
{

}

Candidate::Candidate(const Jid &proxy, const QString &cid, quint16 localPreference) :
    d(new Private)
{
    d->cid = cid;
    d->jid = proxy;
    d->priority = (ProxyPreference << 16) + localPreference;
    d->type = Proxy;
    d->state = Probing;
}

Candidate::Candidate(const QString &host, quint16 port, const QString &cid, Type type, quint16 localPreference) :
    d(new Private)
{
    d->cid = cid;
    d->host = host;
    d->port = port;
    d->type = type;
    static const int priorities[] = {0, ProxyPreference, TunnelPreference, AssistedPreference, DirectPreference};
    if (type >= Type(0) && type <= Direct) {
        d->priority = (priorities[type] << 16) + localPreference;
    } else {
        d->priority = 0;
    }
    d->state = New;
}

Candidate::~Candidate()
{

}

Candidate::Type Candidate::type() const
{
    return d->type;
}

QString Candidate::cid() const
{
    return d->cid;
}

Jid Candidate::jid() const
{
    return d->jid;
}

QString Candidate::host() const
{
    return d->host;
}

void Candidate::setHost(const QString &host)
{
    d->host = host;
}

quint16 Candidate::port() const
{
    return d->port;
}

void Candidate::setPort(quint16 port)
{
    d->port = port;
}

Candidate::State Candidate::state() const
{
    return d->state;
}

void Candidate::setState(Candidate::State s)
{
    d->state = s;
}

quint32 Candidate::priority() const
{
    return d->priority;
}

QDomElement Candidate::toXml(QDomDocument *doc) const
{
    auto e = doc->createElement(QStringLiteral("candidate"));
    e.setAttribute(QStringLiteral("cid"), d->cid);
    if (d->type == Proxy) {
        e.setAttribute(QStringLiteral("jid"), d->jid.full());
    }
    if (!d->host.isEmpty() && d->port) {
        e.setAttribute(QStringLiteral("host"), d->host);
        e.setAttribute(QStringLiteral("port"), d->port);
    }
    e.setAttribute(QStringLiteral("priority"), d->priority);

    static const char *types[] = {"proxy", "tunnel", "assisted"}; // same order as in enum
    if (d->type && d->type < Direct) {
        e.setAttribute(QStringLiteral("type"), QLatin1String(types[d->type - 1]));
    }
    return e;
}

void Candidate::connectToHost(std::function<void(bool)> callback)
{
    // TODO negotiate socks5 connection to host and port
    Q_UNUSED(callback)
}

class Transport::Private : public QSharedData {
public:
    enum PendingActions {
        NewCandidate    = 1,
        CandidateUsed   = 2,
        CandidateError  = 4,
        Activated       = 8,
        ProxyError      = 16
    };

    Transport *q = NULL;
    Pad::Ptr pad;
    bool meCreator = true; // content.content is local side
    bool connectionStarted = false; // where start() was called
    bool waitingAck = true;
    bool aborted = false;
    bool remoteReportedCandidateError = false;
    bool localReportedCandidateError = false;
    bool proxyDiscoveryInProgress = false; // if we have valid proxy requests
    quint16 pendingActions;
    int proxiesInDiscoCount = 0;
    quint32 minimalPriority = 0;
    Application *application = nullptr;
    QMap<QString,Candidate> localCandidates; // cid to candidate mapping
    QMap<QString,Candidate> remoteCandidates;
    Candidate localUsedCandidate; // we received "candidate-used" for this candidate from localCandidates list
    Candidate remoteUsedCandidate; // we sent "candidate-used" for this candidate from remoteCandidates list
    QString dstaddr;
    QString sid;
    Transport::Mode mode = Transport::Tcp;

    inline Jid remoteJid() const
    {
        return pad->session()->peer();
    }

    QString generateCid() const
    {
        QString cid;
        do {
            cid = QString("%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
        } while (localCandidates.contains(cid) || remoteCandidates.contains(cid));
        return cid;
    }

    bool isDup(const Candidate &c) const
    {
        for (auto const &rc: remoteCandidates) {
            if (c.host() == rc.host() && c.port() == rc.port()) {
                return true;
            }
        }
        return false;
    }

    void tryConnectToRemoteCandidate()
    {
        if (!connectionStarted) {
            return; // will come back later
        }
        quint64 priority = 0;
        Candidate candidate;
        for (auto &c: remoteCandidates) {
            if ((c.state() == Candidate::New || c.state() == Candidate::Probing) && c.priority() > priority) {
                candidate = c;
                priority = c.priority();
            }
        }
        if (candidate && candidate.state() == Candidate::Probing) {
            return; // already trying connection
        }
        // TODO start connecting to the candidate here
    }

    bool hasUnaknowledgedLocalCandidates() const
    {
        // now ensure all local were sent to remote and no hope left
        if (proxyDiscoveryInProgress) {
            return true;
        }
        // Note: When upnp support is added we have one more check here

        // now local candidates
        for (const auto &c: localCandidates) {
            auto s = c.state();
            if (s == Candidate::Probing || s == Candidate::New || s == Candidate::Unacked) {
                return true;
            }
        }

        return false;
    }

    void checkAndFinishNegotiation()
    {
        // Why we can't send candidate-used/error right when this happens:
        // so the situation: we discarded all remote candidates (failed to connect)
        // but we have some local candidates which are still in Probing state (upnp for example)
        // if we send candidate-error while we have unsent candidates this may trigger transport failure.
        // So for candidate-error two conditions have to be met 1) all remote failed 2) all local were sent no more
        // local candidates are expected to be discovered

        if (!connectionStarted) {
            return; // we can't finish anything in this state. Only Connecting is acceptable
        }

        // sort out already handled states or states which will bring us here a little later
        if (waitingAck || pendingActions || hasUnaknowledgedLocalCandidates())
        {
            // waitingAck some query waits for ack and in the callback this func will be called again
            // pendingActions means we reported to app we have data to send but the app didn't take this data yet,
            // but as soon as it's taken it will switch to waitingAck.
            // And with unacknowledged local candidates we can't send used/error as well as report connected()/failure()
            // until tried them all
            return;
        }

        // if we already sent used/error. In other words if we already have finished local part of negotiation
        if (localReportedCandidateError || remoteUsedCandidate) {
            // maybe it's time to report connected()/failure()
            if (remoteReportedCandidateError || localUsedCandidate) {
                // so remote seems to be finished too.
                // tell application about it and it has to change its state immediatelly
                if (localUsedCandidate || remoteUsedCandidate) {
                    auto c = localUsedCandidate? localUsedCandidate : remoteUsedCandidate;
                    if (c.state() != Candidate::Active) {
                        if (c.type() == Candidate::Proxy) {
                            // If it's proxy, first it has to be activated
                            if (localUsedCandidate) {
                                // it's our side who proposed proxy. so we have to connect to it and activate
                                c.connectToHost([this](bool success){
                                    if (success) {
                                        pendingActions |= Private::Activated;
                                    } else {
                                        pendingActions |= Private::ProxyError;
                                    }
                                    emit q->updated();
                                });
                            } // else so it's remote proxy. let's just wait for <activated> from remote
                        } else {
                            c.setState(Candidate::Active);
                        }
                    }
                    if (c.state() == Candidate::Active) {
                        emit q->connected();
                    }
                } else { // both sides reported candidate error
                    emit q->failed();
                }
            } // else we have to wait till remote reports its status
            return;
        }

        // if we are here then neither candidate-used nor candidate-error was sent to remote,
        // but we can send it now.
        // first let's check if we can send candidate-used
        bool allRemoteDiscarded = true;
        bool hasConnectedRemoteCandidate = false;
        for (const auto &c: remoteCandidates) {
            auto s = c.state();
            if (s != Candidate::Discarded) {
                allRemoteDiscarded = false;
            }
            if (s == Candidate::Pending) { // connected but not yet sent
                hasConnectedRemoteCandidate = true;
            }
        }

        // if we have connection to remote candidate it's time to send it
        if (hasConnectedRemoteCandidate) {
            pendingActions |= Private::CandidateUsed;
            emit q->updated();
            return;
        }

        if (allRemoteDiscarded) {
            pendingActions |= Private::CandidateError;
            emit q->updated();
            return;
        }

        // apparently we haven't connected anywhere but there are more remote candidates to try
    }

    // take used-candidate with highest priority and discard all with lower. also update used candidates themselves
    void updateMinimalPriority() {
        quint32 prio = 0;
        if (localUsedCandidate && minimalPriority < localUsedCandidate.priority() && localUsedCandidate.state() != Candidate::Discarded) {
            prio = localUsedCandidate.priority();
        } else if (remoteUsedCandidate && minimalPriority < remoteUsedCandidate.priority() && remoteUsedCandidate.state() != Candidate::Discarded) {
            prio = remoteUsedCandidate.priority();
        }
        if (prio < minimalPriority) {
            return;
        }
        for (auto &c: localCandidates) {
            if (c.priority() < prio && c.state() != Candidate::Discarded) {
                c.setState(Candidate::Discarded);
            }
        }
        for (auto &c: remoteCandidates) {
            if (c.priority() < prio && c.state() != Candidate::Discarded) {
                c.setState(Candidate::Discarded);
            }
        }
        prio >>= 16;
        if (proxyDiscoveryInProgress && prio > Candidate::ProxyPreference) {
            // all proxies do no make sense anymore. we have successful higher priority candidate
            proxyDiscoveryInProgress = false;
        }
        // if we discarded "used" candidates then reset them to invalid
        if (localUsedCandidate && localUsedCandidate.state() == Candidate::Discarded) {
            localUsedCandidate = Candidate();
        }
        if (remoteUsedCandidate && remoteUsedCandidate.state() == Candidate::Discarded) {
            remoteUsedCandidate = Candidate();
        }
        if (localUsedCandidate && remoteUsedCandidate) {
            if (meCreator) {
                // i'm initiator. see 2.4.4
                localUsedCandidate.setState(Candidate::Discarded);
                localUsedCandidate = Candidate();
            } else {
                remoteUsedCandidate.setState(Candidate::Discarded);
                remoteUsedCandidate = Candidate();
            }
        }
    }
};

Transport::Transport(const TransportManagerPad::Ptr &pad) :
    d(new Private)
{
    d->q = this;
    d->pad = pad.staticCast<Pad>();
    connect(pad->manager(), &TransportManager::abortAllRequested, this, [this](){
        d->aborted = true;
        emit failed();
    });
}

Transport::Transport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl) :
    Transport::Transport(pad)
{
    d->meCreator = false;
    d->dstaddr = transportEl.attribute(QStringLiteral("dstaddr"));
    d->sid = transportEl.attribute(QStringLiteral("sid"));
    if (d->sid.isEmpty() || !update(transportEl)) {
        d.reset();
        return;
    }
}

Transport::~Transport()
{

}

TransportManagerPad::Ptr Transport::pad() const
{
    return d->pad.staticCast<TransportManagerPad>();
}

void Transport::prepare()
{
    auto m = static_cast<Manager*>(d->pad->manager());
    if (d->meCreator) {
        d->sid = d->pad->generateSid();
    }
    d->pad->registerSid(d->sid);

    auto serv = m->socksServ();
    if (serv) {
        for(auto const &h: serv->hostList()) {
            Candidate c(h, serv->port(), d->generateCid(), Candidate::Direct);
            if (!d->isDup(c)) {
                d->localCandidates.insert(c.cid(), c);
            }
        }
    }

    Jid proxy = m->userProxy();
    if (proxy.isValid()) {
        Candidate c(proxy, d->generateCid());
        if (!d->isDup(c)) {
            d->localCandidates.insert(c.cid(), c);
        }
    }

    d->proxyDiscoveryInProgress = true;
    QList<QSet<QString>> featureOptions = {{"http://jabber.org/protocol/bytestreams"}};
    d->pad->session()->manager()->client()->serverInfoManager()->
            queryServiceInfo(QStringLiteral("proxy"),
                             QStringLiteral("bytestreams"),
                             featureOptions,
                             QRegExp("proxy.*|socks.*|stream.*|s5b.*"),
                             ServerInfoManager::SQ_CheckAllOnNoMatch,
                             [this](const QList<DiscoItem> &items)
    {
        if (!d->proxyDiscoveryInProgress) { // check if new results are ever/still expected
            // seems like we have successful connection via higher priority channel. so nobody cares about proxy
            return;
        }
        auto m = static_cast<Manager*>(d->pad->manager());
        Jid userProxy = m->userProxy();

        // queries proxy's host/port and sends the candidate to remote
        auto queryProxy = [this](const Jid &j, const QString &cid) {
            d->proxiesInDiscoCount++;
            auto query = new JT_S5B(d->pad->session()->manager()->client()->rootTask());
            connect(query, &JT_S5B::finished, this, [this,query,cid](){
                if (!d->proxyDiscoveryInProgress) {
                    return;
                }
                bool candidateUpdated = false;
                auto c = d->localCandidates.value(cid);
                if (c && c.state() == Candidate::Probing) {
                    auto sh = query->proxyInfo();
                    if (query->success() && !sh.host().isEmpty() && sh.port()) {
                        // it can be discarded by this moment (e.g. got success on a higher priority candidate).
                        // so we have to check.
                        c.setHost(sh.host());
                        c.setPort(sh.port());
                        c.setState(Candidate::New);
                        candidateUpdated = true;
                        d->pendingActions |= Private::NewCandidate;
                    } else {
                        c.setState(Candidate::Discarded);
                    }
                }
                d->proxiesInDiscoCount--;
                if (!d->proxiesInDiscoCount) {
                    d->proxyDiscoveryInProgress = false;
                }
                if (candidateUpdated) {
                    emit updated();
                } else if (!d->proxiesInDiscoCount) {
                    // it's possible it was our last hope and probaby we have to send candidate-error now.
                    d->checkAndFinishNegotiation();
                }
            });
            query->requestProxyInfo(j);
            query->go(true);
        };

        bool userProxyFound = !userProxy.isValid();
        for (const auto i: items) {
            int localPref = 0;
            if (!userProxyFound && i.jid() == userProxy) {
                localPref = 1;
                userProxyFound = true;
            }
            Candidate c(i.jid(), d->generateCid(), localPref);
            d->localCandidates.insert(c.cid(), c);

            queryProxy(i.jid(), c.cid());
        }
        if (!userProxyFound) {
            Candidate c(userProxy, d->generateCid(), 1);
            d->localCandidates.insert(c.cid(), c);
            queryProxy(userProxy, c.cid());
        } else if (items.count() == 0) {
            // seems like we don't have any proxy
            d->proxyDiscoveryInProgress = false;
            d->checkAndFinishNegotiation();
        }
    });

    // TODO nat-assisted candidates..
    emit updated();
}

// we got content acceptance from any side and not can connect
void Transport::start()
{
    d->connectionStarted = true;
    d->tryConnectToRemoteCandidate();
}

bool Transport::update(const QDomElement &transportEl)
{
    // we can just on type of elements in transport-info
    // so return as soon as any type handled. Though it leaves a room for  remote to send invalid transport-info
    QString contentTag(QStringLiteral("candidate"));
    int candidatesAdded = 0;
    for(QDomElement ce = transportEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        Candidate c(ce);
        if (!c) {
            return false;
        }
        d->remoteCandidates.insert(c.cid(), c); // TODO check for collisions!
        candidatesAdded++;
    }
    if (candidatesAdded) {
        d->pendingActions &= ~Private::CandidateError;
        d->localReportedCandidateError = false;
        QTimer::singleShot(0, this, [this](){
            d->tryConnectToRemoteCandidate();
        });
        return true;
    }

    QDomElement el = transportEl.firstChildElement(QStringLiteral("candidate-used"));
    if (!el.isNull()) {
        auto cUsed = d->localCandidates.value(el.attribute(QStringLiteral("cid")));
        if (!cUsed) {
            return false;
        }
        cUsed.setState(Candidate::Accepted);
        d->localUsedCandidate = cUsed;
        d->updateMinimalPriority();
        QTimer::singleShot(0, this, [this](){ d->checkAndFinishNegotiation(); });
        return true;
    }

    el = transportEl.firstChildElement(QStringLiteral("candidate-error"));
    if (!el.isNull()) {
        d->remoteReportedCandidateError = true;
        for (auto &c: d->localCandidates) {
            if (c.state() == Candidate::Pending) {
                c.setState(Candidate::Discarded);
            }
        }
        QTimer::singleShot(0, this, [this](){ d->checkAndFinishNegotiation(); });
        return true;
    }

    el = transportEl.firstChildElement(QStringLiteral("activated"));
    if (!el.isNull()) {
        auto c = d->localCandidates.value(el.attribute(QStringLiteral("cid")));
        if (!c) {
            return false;
        }
        if (!(c.type() == Candidate::Proxy && c.state() == Candidate::Accepted && c == d->localUsedCandidate)) {
            qDebug("Received <activated> on a candidate in an inappropriate state. Ignored.");
            return true;
        }
        c.setState(Candidate::Active);
        QTimer::singleShot(0, this, [this](){ emit connected(); });
        return true;
    }

    el = transportEl.firstChildElement(QStringLiteral("proxy-error"));
    if (!el.isNull()) {
        auto c = d->localCandidates.value(el.attribute(QStringLiteral("cid")));
        if (!c) {
            return false;
        }
        if (c != d->localUsedCandidate || c.state() != Candidate::Accepted) {
            qDebug("Received <proxy-error> on a candidate in an inappropriate state. Ignored.");
            return true;
        }

        // if we got proxy-error then the transport has to be considered failed according to spec
        // so never send proxy-error while we have unaknowledged local non-proxy candidates,
        // but we have to follow the standard.

        // Discard everything
        for (auto &c: d->localCandidates) {
            c.setState(Candidate::Discarded);
        }
        for (auto &c: d->remoteCandidates) {
            c.setState(Candidate::Discarded);
        }
        d->proxyDiscoveryInProgress = false;
        // TODO do the same for upnp when implemented

        QTimer::singleShot(0, this, [this](){ emit failed(); });
        return true;
    }

    return false;
}

bool Transport::hasUpdates() const
{
    return isValid() && d->pendingActions;
}

OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate()
{
    OutgoingTransportInfoUpdate upd;
    if (!isValid()) {
        return upd;
    }

    auto doc = d->pad->session()->manager()->client()->doc();

    QDomElement tel = doc->createElementNS(NS, "transport");
    tel.setAttribute(QStringLiteral("sid"), d->sid);
    if (d->meCreator && d->mode != Tcp) {
        tel.setAttribute(QStringLiteral("mode"), "udp");
    }

    if (d->pendingActions & Private::NewCandidate) {
        d->pendingActions &= ~Private::NewCandidate;
        bool useProxy = false;
        QList<Candidate> candidatesToSend;
        for (auto &c: d->localCandidates) {
            if (c.state() != Candidate::New) {
                continue;
            }
            if (c.type() == Candidate::Proxy) {
                useProxy = true;
            }
            tel.appendChild(c.toXml(doc));
            candidatesToSend.append(c);
            c.setState(Candidate::Unacked);
        }
        if (useProxy) {
            QString dstaddr = QCryptographicHash::hash((d->sid +
                                                        d->pad->session()->me().full() +
                                                        d->pad->session()->peer().full()).toUtf8(),
                                                       QCryptographicHash::Sha1);
            tel.setAttribute(QStringLiteral("dstaddr"), dstaddr);
        }
        if (!candidatesToSend.isEmpty()) {
            d->waitingAck = true;
            upd = OutgoingTransportInfoUpdate{tel, [this, candidatesToSend]() mutable {
                d->waitingAck = false;
                for (auto &c: candidatesToSend) {
                    if (c.state() == Candidate::Unacked) {
                        c.setState(Candidate::Pending);
                    }
                }
                d->checkAndFinishNegotiation();
            }};
        }
    } else if (d->pendingActions & Private::CandidateUsed) {
        d->pendingActions &= ~Private::NewCandidate;
        // we should have the only remote candidate in Pending state
        for (auto &c: d->remoteCandidates) {
            if (c.state() != Candidate::Pending) {
                continue;
            }
            auto el = tel.appendChild(doc->createElement(QStringLiteral("candidate-used"))).toElement();
            el.setAttribute(QStringLiteral("cid"), c.cid());
            c.setState(Candidate::Unacked);

            d->waitingAck = true;
            upd = OutgoingTransportInfoUpdate{tel, [this, c]() mutable {
                d->waitingAck = false;
                if (c.state() == Candidate::Unacked) {
                    c.setState(Candidate::Accepted);
                    d->remoteUsedCandidate = c;
                }
                d->checkAndFinishNegotiation();
            }};

            break;
        }
    } else if (d->pendingActions & Private::CandidateError) {
        d->pendingActions &= ~Private::CandidateError;
        // we are here because all remote are already in Discardd state
        tel.appendChild(doc->createElement(QStringLiteral("candidate-error")));
        d->waitingAck = true;
        upd = OutgoingTransportInfoUpdate{tel, [this]() mutable {
            d->waitingAck = false;
            d->localReportedCandidateError = true;
            d->checkAndFinishNegotiation();
        }};
    } else if (d->pendingActions & Private::Activated) {
        d->pendingActions &= ~Private::Activated;
        if (d->localUsedCandidate) {
            auto cand = d->localUsedCandidate;
            auto el = tel.appendChild(doc->createElement(QStringLiteral("activated"))).toElement();
            el.setAttribute(QStringLiteral("cid"), cand.cid());
            d->waitingAck = true;
            upd = OutgoingTransportInfoUpdate{tel, [this, cand]() mutable {
                d->waitingAck = false;
                if (cand.state() != Candidate::Accepted || d->localUsedCandidate != cand) {
                    return; // seems like state was changed while we was waiting for an ack
                }
                cand.setState(Candidate::Active);
                d->checkAndFinishNegotiation();
            }};
        }
    } else if (d->pendingActions & Private::ProxyError) {
        // we send proxy error only for local proxy
        d->pendingActions &= ~Private::ProxyError;
        if (d->localUsedCandidate) {
            auto cand = d->localUsedCandidate;
            tel.appendChild(doc->createElement(QStringLiteral("proxy-error")));
            d->waitingAck = true;
            upd = OutgoingTransportInfoUpdate{tel, [this, cand]() mutable {
                d->waitingAck = false;
                if (cand.state() != Candidate::Accepted || d->localUsedCandidate != cand) {
                    return; // seems like state was changed while we was waiting for an ack
                }
                cand.setState(Candidate::Discarded);
                d->localUsedCandidate = Candidate();
                emit failed();
            }};
        }
    }

    return upd; // TODO
}

bool Transport::isValid() const
{
    return d != nullptr;
}

Transport::Features Transport::features() const
{
    return Features(HardToConnect | Reliable | Fast);
}

QString Transport::sid() const
{
    return d->sid;
}

bool Transport::incomingConnection(SocksClient *sc, const QString &key)
{
    // incoming direct connection
#if 0
    if(!d->allowIncoming) {
        sc->requestDeny();
        sc->deleteLater();
        return;
    }
    if(d->mode == Transport::Udp)
        sc->grantUDPAssociate("", 0);
    else
        sc->grantConnect();
    e->relatedServer = static_cast<S5BServer *>(sender());
    e->i->setIncomingClient(sc);
#endif
    return false;
}

//----------------------------------------------------------------
// Manager
//----------------------------------------------------------------

class Manager::Private
{
public:
    XMPP::Jingle::Manager *jingleManager = nullptr;
    S5BServer *serv = nullptr;

    // FIMME it's reuiqred to split transports by direction otherwise we gonna hit conflicts.
    // jid,transport-sid -> transport mapping
    QSet<QPair<Jid,QString>> sids;
    QHash<QString,Transport*> key2transport;
    Jid proxy;
};

Manager::Manager(QObject *parent) :
    TransportManager(parent),
    d(new Private)
{
}

Manager::~Manager()
{
    d->jingleManager->unregisterTransport(NS);
}

Transport::Features Manager::features() const
{
    return Transport::Reliable | Transport::Fast;
}

void Manager::setJingleManager(XMPP::Jingle::Manager *jm)
{
    d->jingleManager = jm;
}

QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad)
{
    return QSharedPointer<XMPP::Jingle::Transport>(new Transport(pad));
}

QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl)
{
    auto t = new Transport(pad, transportEl);
    QSharedPointer<XMPP::Jingle::Transport> ret(t);
    if (t->isValid()) {
        return ret;
    }
    return QSharedPointer<XMPP::Jingle::Transport>();
}

TransportManagerPad* Manager::pad(Session *session)
{
    return new Pad(this, session);
}

void Manager::closeAll()
{
    emit abortAllRequested();
}

void Manager::setServer(S5BServer *serv)
{
    if(d->serv) {
        d->serv->unlink(this);
        d->serv = nullptr;
    }

    if(serv) {
        d->serv = serv;
        d->serv->link(this);
    }
}

bool Manager::incomingConnection(SocksClient *client, const QString &key)
{
    auto t = d->key2transport.value(key);
    if (t) {
        return t->incomingConnection(client, key);
    }
    return false;
}

QString Manager::generateSid(const Jid &remote)
{
    QString sid;
    QPair<Jid,QString> key;
    do {
        sid = QString("s5b_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
        key = qMakePair(remote, sid);
    } while (d->sids.contains(key));
    return sid;
}

void Manager::registerSid(const Jid &remote, const QString &sid)
{
    d->sids.insert(qMakePair(remote, sid));
}

S5BServer *Manager::socksServ() const
{
    return d->serv;
}

Jid Manager::userProxy() const
{
    return d->proxy;
}

//----------------------------------------------------------------
// Pad
//----------------------------------------------------------------
Pad::Pad(Manager *manager, Session *session) :
    _manager(manager),
    _session(session)
{

}

QString Pad::ns() const
{
    return NS;
}

Session *Pad::session() const
{
    return _session;
}

TransportManager *Pad::manager() const
{
    return _manager;
}

QString Pad::generateSid() const
{
    return _manager->generateSid(_session->peer());
}

void Pad::registerSid(const QString &sid)
{
    return _manager->registerSid(_session->peer(), sid);
}

} // namespace S5B
} // namespace Jingle
} // namespace XMPP
