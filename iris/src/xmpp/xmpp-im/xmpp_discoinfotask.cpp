/*
 * Copyright (C) 2001, 2002  Justin Karneges
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

#include <QDomElement>
#include <QString>
#include <QTimer>

#include "xmpp_task.h"
#include "xmpp/jid/jid.h"
#include "xmpp_discoitem.h"
#include "xmpp_discoinfotask.h"
#include "xmpp_xmlcommon.h"
#include "xmpp_client.h"
#include "xmpp_caps.h"

using namespace XMPP;

class DiscoInfoTask::Private
{
public:
    Private() : allowCache(true) { }

    bool allowCache;
    Jid jid;
    QString node;
    DiscoItem::Identity ident;
    DiscoItem item;
};

DiscoInfoTask::DiscoInfoTask(Task *parent)
: Task(parent)
{
    d = new Private;
}

DiscoInfoTask::~DiscoInfoTask()
{
    delete d;
}

void DiscoInfoTask::setAllowCache(bool allow)
{
    d->allowCache = allow;
}

void DiscoInfoTask::get(const DiscoItem &item)
{
    DiscoItem::Identity id;
    if ( item.identities().count() == 1 )
        id = item.identities().first();
    get(item.jid(), item.node(), id);
}

void DiscoInfoTask::get (const Jid &j, const QString &node, DiscoItem::Identity ident)
{
    d->item = DiscoItem(); // clear item

    d->jid = j;
    d->node = node;
    d->ident = ident;
}


/**
 * Original requested jid.
 * Is here because sometimes the responder does not include this information
 * in the reply.
 */
const Jid& DiscoInfoTask::jid() const
{
    return d->jid;
}

/**
 * Original requested node.
 * Is here because sometimes the responder does not include this information
 * in the reply.
 */
const QString& DiscoInfoTask::node() const
{
    return d->node;
}

const DiscoItem &DiscoInfoTask::item() const
{
    return d->item;
}

void DiscoInfoTask::onGo ()
{
    if (d->allowCache && client()->capsManager()->isEnabled()) {
        d->item = client()->capsManager()->disco(d->jid);
        if (!d->item.features().isEmpty() || d->item.identities().count()) {
            QTimer::singleShot(0, this, SLOT(cachedReady())); // to be consistent with network requests
            return;
        }
    }

    QDomElement iq = createIQ(doc(), "get", d->jid.full(), id());
    QDomElement query = doc()->createElement("query");
    query.setAttribute("xmlns", "http://jabber.org/protocol/disco#info");

    if ( !d->node.isEmpty() )
        query.setAttribute("node", d->node);

    if ( !d->ident.category.isEmpty() && !d->ident.type.isEmpty() ) {
        QDomElement i = doc()->createElement("item");

        i.setAttribute("category", d->ident.category);
        i.setAttribute("type", d->ident.type);
        if ( !d->ident.name.isEmpty() )
            i.setAttribute("name", d->ident.name);

        query.appendChild( i );

    }

    iq.appendChild(query);
    send(iq);
}

void DiscoInfoTask::cachedReady()
{
    d->item.setJid( d->jid );
    setSuccess();
}

bool DiscoInfoTask::take(const QDomElement &x)
{
    if(!iqVerify(x, d->jid, id()))
        return false;

    if(x.attribute("type") == "result") {
        d->item = DiscoItem::fromDiscoInfoResult(queryTag(x));
        d->item.setJid( d->jid );
        if (d->allowCache && client()->capsManager()->isEnabled()) {
            client()->capsManager()->updateDisco(d->jid, d->item);
        }

        setSuccess();
    }
    else {
        setError(x);
    }

    return true;
}


