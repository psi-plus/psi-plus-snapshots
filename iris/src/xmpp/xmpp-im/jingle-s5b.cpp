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
    quint16 priority;
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
    quint16 priority = priorityStr.toUShort(&ok);
    if (!ok) {
        return; // make the whole candidate invalid
    }
    QString cid = el.attribute(QStringLiteral("cid"));
    if (cid.isEmpty()) {
        return;
    }

    QString ct = el.attribute(QStringLiteral("type"));
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

Candidate::~Candidate()
{

}

class Transport::Private {
public:
    Pad::Ptr pad;
    bool aborted = false;
    Application *application = nullptr;
    QList<Candidate> localCandidates;
    QList<Candidate> remoteCandidates;
    QString dstaddr;
    QString sid;
    Transport::Mode mode = Transport::Tcp;
    Jid proxy;

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
};

Transport::Transport(const TransportManagerPad::Ptr &pad) :
    d(new Private)
{
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

void Transport::start()
{

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
        d->remoteCandidates.append(c); // TODO check for collisions!
    }
    // TODO handle "candidate-used" and "activted"
    return true;
}

Jingle::Action Transport::outgoingUpdateType() const
{
    return Jingle::NoAction; // TODO
}

QDomElement Transport::takeOutgoingUpdate()
{
    if (!isValid() || !d->application) {
        return QDomElement();
    }
    if (d->application->creator() == d->pad->session()->role()) { // I'm the creator
        if (d->application->state() == State::Created) {
            auto doc = d->pad->session()->manager()->client()->doc();
            auto tel = doc->createElementNS(NS, "transport");
            tel.setAttribute(QStringLiteral("sid"), d->sid);
            if (d->mode != Tcp) {
                tel.setAttribute(QStringLiteral("mode"), "udp");
            }
            if (d->proxy.isValid()) {
                QString dstaddr = QCryptographicHash::hash((d->sid +
                                                           d->pad->session()->initiator().full() +
                                                           d->pad->session()->responder().full()).toUtf8(),
                                                           QCryptographicHash::Sha1);
                tel.setAttribute(QStringLiteral("dstaddr"), dstaddr);
            }
        }
    } else {

    }
    return QDomElement(); // TODO
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
    // TODO
    Q_UNUSED(client);
    Q_UNUSED(key);
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
