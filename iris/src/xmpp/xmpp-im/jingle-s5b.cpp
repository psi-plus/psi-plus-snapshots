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

class Transport::Private : public QSharedData {
public:
    enum PendingActions {
        NewCandidate,
        CandidateUsed,
        CandidateError,
        Activated,
        ProxyError
    };

    Transport *q = NULL;
    Pad::Ptr pad;
    bool aborted = false;
    bool signalNegotiated = false;
    bool remoteReportedCandidateError = false;
    bool localReportedCandidateError = false;
    bool proxyDiscoveryInProgress = false;
    quint16 pendingActions;
    int proxiesInDiscoCount = 0;
    quint32 minimalPriority = 0;
    Application *application = nullptr;
    QMap<QString,Candidate> localCandidates; // cid to candidate mapping
    QMap<QString,Candidate> remoteCandidates;
    QSet<QPair<QString,Origin>> signalingCandidates; // origin here is session role. so for remote it's != session->role
    Candidate localUsedCandidate; // we received "candidate-used" for this candidate
    Candidate remoteUsedCandidate;
    QString dstaddr;
    QString sid;
    Transport::Mode mode = Transport::Tcp;

    bool amISender() const
    {
        Q_ASSERT(application);
        auto senders = application->senders();
        return senders == Origin::Both || senders == application->creator();
    }

    bool amIReceiver() const
    {
        Q_ASSERT(application);
        auto senders = application->senders();
        return senders == Origin::Both || senders == negateOrigin(application->creator());
    }

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

    void updateSelfState()
    {
        // TODO code below is from handler of "candidate-used". it has to be updated
        bool hasMoreCandidates = false;
        for (auto &c: localCandidates) {
            auto s = c.state();
            if (s < Candidate::Pending && c.priority() > localUsedCandidate.priority()) {
                hasMoreCandidates = true;
                continue; // we have more high priority candidates to be handled by remote
            }
            c.setState(Candidate::Discarded);
        }

        if (hasMoreCandidates) {
            return;
        }

        // let's check remote candidates too before we decide to use this local candidate
        for (auto &c: remoteCandidates) {
            auto s = c.state();
            if (c.priority() > localUsedCandidate.priority() && (s == Candidate::Pending ||
                                                    s == Candidate::Probing ||
                                                    s == Candidate::New)) {
                hasMoreCandidates = true;
                continue; // we have more high priority candidates to be handled by remote
            } else if (c.priority() == localUsedCandidate.priority() && s == Candidate::Unacked &&
                       pad->session()->role() == Origin::Initiator) {
                hasMoreCandidates = true;
                continue; // see 2.4 Completing the Negotiation (p.4)
            }
            c.setState(Candidate::Discarded); // TODO stop any probing as well
        }

        if (hasMoreCandidates) {
            return;
        }

        // seems like we don't have better candidates,
        // so we are going to use the d->localUsedCandidate
        signalNegotiated = true;
    }

    void tryConnectToRemoteCandidate()
    {
        if (application->state() != State::Connecting) {
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

void Transport::setApplication(Application *app)
{
    d->application = app;
    if (app->creator() == d->pad->session()->role()) { // I'm a creator
        d->sid = d->pad->generateSid();
    }
    d->pad->registerSid(d->sid);
}

void Transport::prepare()
{
    auto m = static_cast<Manager*>(d->pad->manager());

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
        if (d->minimalPriority >= (Candidate::TunnelPreference << 16)) {
            // seems like we have successful connection via higher priority channel. so nobody cares about proxy
            d->proxyDiscoveryInProgress = false;
            return;
        }
        auto m = static_cast<Manager*>(d->pad->manager());
        Jid userProxy = m->userProxy();

        // queries proxy's host/port and sends the candidate to remote
        auto queryProxy = [this](const Jid &j, const QString &cid) {
            d->proxiesInDiscoCount++;
            auto query = new JT_S5B(d->pad->session()->manager()->client()->rootTask());
            connect(query, &JT_S5B::finished, this, [this,query,cid](){
                bool candidateUpdated = false;
                if (query->success()) {
                    auto sh = query->proxyInfo();
                    auto c = d->localCandidates.value(cid);
                    if (c && c.state() == Candidate::Probing) { // it can discarded by this moment. so we have to check.
                        c.setHost(sh.host());
                        c.setPort(sh.port());
                        c.setState(Candidate::New);
                        candidateUpdated = true;
                    }
                }
                d->proxiesInDiscoCount--;
                if (!d->proxiesInDiscoCount) {
                    d->proxyDiscoveryInProgress = false;
                }
                if (candidateUpdated) {
                    d->pendingActions |= Private::NewCandidate;
                    emit updated();
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
            // but it's possible it was our last hope and probaby we have to send candidate-error now.
            // so the situation: we discarded all remote candidates (failed to connect)
            // and all our candidates were already sent to remote
            // if we send candidate-error while we have unsent candidates this may trigger transport failure.
            // So for candidate-error two conditions have to be met 1) all remote failed 2) all local were sent no more
            // local candidates are expected to be discovered
            for (const auto &c: d->remoteCandidates) {
                if (c.state() != Candidate::Discarded) {
                    // we have other cadidates to handle. so we don't need candidate-error to be sent to remote yet
                    return;
                }
            }
            // now ensure all local were sent to remote and no hope left
            for (const auto &c: d->localCandidates) {
                auto s = c.state();
                if (s == Candidate::Probing || s == Candidate::New) {
                    return;
                }
            }
            d->pendingActions |= Private::CandidateError;
        }
    });

    for (auto const &c: d->localCandidates) {
        d->signalingCandidates.insert(QPair<QString,Origin>{c.cid(),d->pad->session()->role()});
    }

    // TODO nat-assisted candidates..
    emit updated();
}

void Transport::start()
{
    // TODO start connecting to remote candidates
}

bool Transport::update(const QDomElement &transportEl)
{
    QString contentTag(QStringLiteral("candidate"));
    bool candidatesAdded = false;;
    for(QDomElement ce = transportEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        Candidate c(ce);
        if (!c) {
            return false;
        }
        d->remoteCandidates.insert(c.cid(), c); // TODO check for collisions!
        candidatesAdded = true;
    }
    if (candidatesAdded) {
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
        cUsed.setState(Candidate::Accepted);
        QTimer::singleShot(0, this, [this](){ d->updateSelfState(); });
    }

    el = transportEl.firstChildElement(QStringLiteral("candidate-error"));
    if (!el.isNull()) {
        d->remoteReportedCandidateError = true;
        QTimer::singleShot(0, this, [this](){ d->updateSelfState(); });
    }

    el = transportEl.firstChildElement(QStringLiteral("activated"));
    if (!el.isNull()) {
        auto c = d->localCandidates.value(el.attribute(QStringLiteral("cid")));
        if (!c) {
            return false;
        }
        c.setState(Candidate::Active);
        QTimer::singleShot(0, this, [this](){ d->updateSelfState(); });
    }

    el = transportEl.firstChildElement(QStringLiteral("proxy-error"));
    if (!el.isNull()) {
        auto c = d->localCandidates.value(el.attribute(QStringLiteral("cid")));
        if (!c) {
            return false;
        }
        c.setState(Candidate::Discarded);
        QTimer::singleShot(0, this, [this](){ d->updateSelfState(); });
    }

    return true;
}

Action Transport::outgoingUpdateType() const
{
    if (isValid() && d->application) {
        // if we are preparing local offer and have at least one candidate, we have to sent it.
        // otherwise it's not first update to remote from this transport, so we have to send just signalling candidates
        if ((d->application->state() == State::PrepareLocalOffer && d->localCandidates.size()) ||
                (d->application->state() > State::PrepareLocalOffer && d->application->state() < State::Finished &&
                 !d->signalingCandidates.isEmpty()))
        {
            return Action::TransportInfo;
        }
    }
    return Action::NoAction; // TODO
}

OutgoingUpdate Transport::takeOutgoingUpdate()
{
    OutgoingUpdate upd;
    State appState;
    if (!isValid() || !d->application || (appState = d->application->state()) == State::Finished) {
        return upd;
    }

    auto sessRole = d->pad->session()->role();
    auto doc = d->pad->session()->manager()->client()->doc();

    if (appState == State::PrepareLocalOffer && !d->localCandidates.isEmpty()) {
        QDomElement tel = doc->createElementNS(NS, "transport");
        tel.setAttribute(QStringLiteral("sid"), d->sid);
        if (d->mode != Tcp) {
            tel.setAttribute(QStringLiteral("mode"), "udp");
        }
        bool useProxy = false;
        QList<Candidate> candidatesToSend;
        for (auto &c: d->localCandidates) {
            if (c.type() == Candidate::Proxy) {
                useProxy = true;
            }
            if (!c.host().isEmpty()) {
                tel.appendChild(c.toXml(doc));
            }
            candidatesToSend.append(c);
            d->signalingCandidates.remove(QPair<QString,Origin>{c.cid(),sessRole});
            c.setState(Candidate::Unacked);
        }
        if (useProxy) {
            QString dstaddr = QCryptographicHash::hash((d->sid +
                                                        d->pad->session()->me().full() +
                                                        d->pad->session()->peer().full()).toUtf8(),
                                                       QCryptographicHash::Sha1);
            tel.setAttribute(QStringLiteral("dstaddr"), dstaddr);
        }
        OutgoingUpdate{tel, [this, candidatesToSend]() mutable {
                for (auto &c: candidatesToSend) {
                    c.setState(Candidate::Pending);
                }
            }}; // FIXME we should update candidates status here
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
