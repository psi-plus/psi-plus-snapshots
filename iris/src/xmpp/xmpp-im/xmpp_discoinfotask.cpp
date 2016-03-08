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

#include "xmpp_task.h"
#include "xmpp/jid/jid.h"
#include "xmpp_discoitem.h"
#include "xmpp_discoinfotask.h"
#include "xmpp_xmlcommon.h"

using namespace XMPP;

class DiscoInfoTask::Private
{
public:
	Private() { }

	QDomElement iq;
	Jid jid;
	QString node;
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
	d->iq = createIQ(doc(), "get", d->jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "http://jabber.org/protocol/disco#info");

	if ( !node.isEmpty() )
		query.setAttribute("node", node);

	if ( !ident.category.isEmpty() && !ident.type.isEmpty() ) {
		QDomElement i = doc()->createElement("item");

		i.setAttribute("category", ident.category);
		i.setAttribute("type", ident.type);
		if ( !ident.name.isEmpty() )
			i.setAttribute("name", ident.name);

		query.appendChild( i );

	}

	d->iq.appendChild(query);
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
	send(d->iq);
}

bool DiscoInfoTask::take(const QDomElement &x)
{
	if(!iqVerify(x, d->jid, id()))
		return false;

	if(x.attribute("type") == "result") {
		d->item = DiscoItem::fromDiscoInfoResult(queryTag(x));
		d->item.setJid( d->jid );

		setSuccess(true);
	}
	else {
		setError(x);
	}

	return true;
}


