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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
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
    TransportManagerPad::Ptr pad;
    Jid remoteJid;
    QList<Candidate> localCandidates;
    QList<Candidate> remoteCandidates;
    QString dstaddr;
    QString sid;
    Transport::Mode mode = Transport::Tcp;
    Transport::Direction direction = Transport::Outgoing;
};

Transport::Transport()
{

}

Transport::~Transport()
{

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

QSharedPointer<XMPP::Jingle::Transport> Transport::createOutgoing(const TransportManagerPad::Ptr &pad, const Jid &to, const QString &transportSid)
{
    auto d = new Private;
    d->pad = pad;
    d->remoteJid = to;
    d->direction = Transport::Outgoing;
    d->mode = Transport::Tcp;
    d->sid = transportSid;

    auto t = new Transport;
    t->d.reset(d);
    return QSharedPointer<XMPP::Jingle::Transport>(t);
}

QSharedPointer<XMPP::Jingle::Transport> Transport::createIncoming(const TransportManagerPad::Ptr &pad, const Jid &from, const QDomElement &transportEl)
{
    auto d = new Private;
    d->pad = pad;
    d->remoteJid = from;
    d->direction = Transport::Incoming;
    d->mode = Transport::Tcp;
    d->dstaddr = transportEl.attribute(QStringLiteral("dstaddr"));
    d->sid = transportEl.attribute(QStringLiteral("sid"));
    if (d->sid.isEmpty()) {
        delete d;
        return QSharedPointer<XMPP::Jingle::Transport>();
    }

    auto t = new Transport;
    t->d.reset(d);
    QSharedPointer<XMPP::Jingle::Transport> st(t);
    if (!st->update(transportEl)) {
        return QSharedPointer<XMPP::Jingle::Transport>();
    }
    return st;
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
    QHash<QPair<Jid,QString>,QSharedPointer<XMPP::Jingle::Transport>> transports;
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
    QString sid;
    QPair<Jid,QString> key;
    Jid to = pad->session()->peer();
    do {
        sid = QString("s5b_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
        key = qMakePair(to, sid);
    } while (d->transports.contains(key));

    auto t = Transport::createOutgoing(pad, to, sid);
    d->transports.insert(key, t);
    return t;
}

QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl)
{
    Jid from = pad->session()->peer();
    auto t = Transport::createIncoming(pad, from, transportEl);
    if (t->isValid()) {
        d->transports.insert(qMakePair(from, t.staticCast<Transport>()->sid()), t); // FIXME collisions??
    }
    return t;
}

TransportManagerPad* Manager::pad(Session *session)
{
    return new Pad(this, session);
}

bool Manager::hasTrasport(const Jid &jid, const QString &sid) const
{
    return d->transports.contains(qMakePair(jid, sid));
}

void Manager::closeAll()
{

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


} // namespace S5B
} // namespace Jingle
} // namespace XMPP
