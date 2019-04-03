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
    Candidate::Type type;
};

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
    this->d = d;
}

Candidate::Candidate(const Candidate &other) :
    d(other.d)
{

}

Candidate::Candidate(const Jid &proxy, const QString &cid) :
    d(new Private)
{
    d->cid = cid;
    d->jid = proxy;
    d->priority = ProxyPreference << 16;
    d->type = Proxy;
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

class Transport::Private {
public:
    Transport *q;
    Pad::Ptr pad;
    bool aborted = false;
    Application *application = nullptr;
    QMap<QString,Candidate> localCandidates; // cid to candidate mapping
    QList<Candidate> pendingLocalCandidates; // not yet sent to remote
    QMap<QString,Candidate> remoteCandidates;
    QSet<QPair<QString,Origin>> signalingCandidates; // origin here is session role. so for remote it's != session->role
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

    void sendLocalCandidate(const Candidate &c)
    {
        if (isDup(c)) {
            return; // seems like remote already sent us it..
        }
        pendingLocalCandidates.append(c);
        emit q->updated();
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

    QList<QSet<QString>> featureOptions = {{"http://jabber.org/protocol/bytestreams"}};
    d->pad->session()->manager()->client()->serverInfoManager()->
            queryServiceInfo(QStringLiteral("proxy"),
                             QStringLiteral("bytestreams"),
                             featureOptions,
                             QRegExp("proxy|socks.*|stream.*"),
                             ServerInfoManager::SQ_CheckAllOnNoMatch,
                             [this](const QList<DiscoItem> &items)
    {
        auto m = static_cast<Manager*>(d->pad->manager());
        Jid userProxy = m->userProxy();

        // queries proxy's host/port and sends the candidate to remote
        auto queryProxy = [this](const Jid &j) {
            auto query = new JT_S5B(d->pad->session()->manager()->client()->rootTask());
            connect(query, &JT_S5B::finished, this, [this,query](){
                if (query->success()) {
                    auto sh = query->proxyInfo();
                    Candidate c(sh.jid(), d->generateCid());
                    c.setHost(sh.host());
                    c.setPort(sh.port());
                    d->sendLocalCandidate(c);
                }
            });
            query->requestProxyInfo(j);
            query->go(true);
        };

        bool userProxyFound = !userProxy.isValid();
        for (const auto i: items) {
            if (!userProxyFound && i.jid() == userProxy) {
                userProxyFound = true;
            }
            queryProxy(i.jid());
        }
        if (!userProxyFound) {
            queryProxy(userProxy);
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
    for(QDomElement ce = transportEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        Candidate c(ce);
        if (!c.isValid()) {
            return false;
        }
        d->remoteCandidates.insert(c.cid(), c); // TODO check for collisions!
    }
    // TODO handle "candidate-used" and "activted"
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

QDomElement Transport::takeOutgoingUpdate()
{
    QDomElement tel;
    if (!isValid() || !d->application) {
        return tel;
    }

    auto appState = d->application->state();
    auto sessRole = d->pad->session()->role();
    auto doc = d->pad->session()->manager()->client()->doc();

    if (appState == State::PrepareLocalOffer && !d->localCandidates.isEmpty()) {
        tel = doc->createElementNS(NS, "transport");
        tel.setAttribute(QStringLiteral("sid"), d->sid);
        if (d->mode != Tcp) {
            tel.setAttribute(QStringLiteral("mode"), "udp");
        }
        bool useProxy = false;
        for (auto const &c: d->localCandidates) {
            if (c.type() == Candidate::Proxy) {
                useProxy = true;
            }
            if (!c.host().isEmpty()) {
                tel.appendChild(c.toXml(doc));
            }
            d->signalingCandidates.remove(QPair<QString,Origin>{c.cid(),sessRole});
        }
        if (useProxy) {
            QString dstaddr = QCryptographicHash::hash((d->sid +
                                                        d->pad->session()->me().full() +
                                                        d->pad->session()->peer().full()).toUtf8(),
                                                       QCryptographicHash::Sha1);
            tel.setAttribute(QStringLiteral("dstaddr"), dstaddr);
        }
    }
    return tel; // TODO
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
