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
#include "xmpp/jid/jid.h"

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

Candidate::Candidate(const QDomElement &el) :
    d(new Private)
{
    bool ok;
    d->host = el.attribute(QStringLiteral("host"));
    d->jid = Jid(el.attribute(QStringLiteral("jid")));
    auto port = el.attribute(QStringLiteral("port"));
    if (!port.isEmpty()) {
        d->port = port.toUShort(&ok);
        if (!ok) {
            return; // make the whole candidate invalid
        }
    }
    auto priority = el.attribute(QStringLiteral("priority"));
    if (!priority.isEmpty()) {
        d->priority = priority.toUShort(&ok);
        if (!ok) {
            return; // make the whole candidate invalid
        }
    }
    d->cid = el.attribute(QStringLiteral("cid"));
}

Candidate::Candidate(const Candidate &other) :
    d(other.d)
{

}

Candidate::~Candidate()
{

}

class Negotiation::Private : public QSharedData {
public:
    QList<Candidate> candidates;
    QString dstaddr;
    QString sid;
    Negotiation::Mode mode;
};

Negotiation::Negotiation(const QDomElement &el) :
    d(new Private)
{
    d->sid = el.attribute(QStringLiteral("sid"));
    // TODO remaining
}

Negotiation::Negotiation(const Negotiation &other) :
    d(other.d)
{

}

Negotiation::~Negotiation()
{

}


} // namespace S5B
} // namespace Jingle
} // namespace XMPP
