/*
 * xmpp_discoitem.cpp
 * Copyright (C) 2003  Justin Karneges
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

#include <QtXml>

#include "xmpp_discoitem.h"

using namespace XMPP;

class XMPP::DiscoItemPrivate : public QSharedData
{
public:
	DiscoItemPrivate()
	{
		action = DiscoItem::None;
	}

	Jid jid;
	QString name;
	QString node;
	DiscoItem::Action action;

	Features features;
	DiscoItem::Identities identities;
	QList<XData> exts;
};

DiscoItem::DiscoItem()
{
	d = new DiscoItemPrivate;
}

DiscoItem::DiscoItem(const DiscoItem &from)
{
	d = new DiscoItemPrivate;
	*this = from;
}

DiscoItem & DiscoItem::operator= (const DiscoItem &from)
{
	d->jid = from.d->jid;
	d->name = from.d->name;
	d->node = from.d->node;
	d->action = from.d->action;
	d->features = from.d->features;
	d->identities = from.d->identities;
	d->exts = from.d->exts;

	return *this;
}

DiscoItem::~DiscoItem()
{

}

AgentItem DiscoItem::toAgentItem() const
{
	AgentItem ai;

	ai.setJid( jid() );
	ai.setName( name() );

	Identity id;
	if ( !identities().isEmpty() )
		id = identities().first();

	ai.setCategory( id.category );
	ai.setType( id.type );

	ai.setFeatures( d->features );

	return ai;
}

void DiscoItem::fromAgentItem(const AgentItem &ai)
{
	setJid( ai.jid() );
	setName( ai.name() );

	Identity id;
	id.category = ai.category();
	id.type = ai.type();
	id.name = ai.name();

	Identities idList;
	idList << id;

	setIdentities( idList );

	setFeatures( ai.features() );
}

QString DiscoItem::capsHash(QCryptographicHash::Algorithm algo) const
{
	QStringList prep;
	DiscoItem::Identities idents = d->identities;
	qSort(idents);

	foreach (const DiscoItem::Identity &id, idents) {
		prep << QString("%1/%2/%3/%4").arg(id.category, id.type, id.lang, id.name);
	}

	QStringList fl = d->features.list();
	qSort(fl);
	prep += fl;

	QMap<QString,XData> forms;
	foreach (const XData &xd, d->exts) {
		if (xd.registrarType().isEmpty()) {
			continue;
		}
		if (forms.contains(xd.registrarType())) {
			return QString(); // ill-formed
		}
		forms.insert(xd.registrarType(), xd);
	}
	foreach (const XData &xd, forms.values()) {
		prep << xd.registrarType();
		QMap <QString, QStringList> values;
		foreach (const XData::Field &f, xd.fields()) {
			if (f.var() == QLatin1String("FORM_TYPE")) {
				continue;
			}
			if (values.contains(f.var())) {
				return QString(); // ill-formed
			}
			QStringList v = f.value();
			if (v.isEmpty()) {
				continue; // maybe it's media-element but xep-115 (1.5) and xep-232 (0.3) are not clear about that.
			}
			qSort(v);
			values[f.var()] = v;
		}
		QMap <QString, QStringList>::ConstIterator it = values.constBegin();
		for (; it != values.constEnd(); ++it) {
			prep += it.key();
			prep += it.value();
		}
	}

	QByteArray ba = QString(prep.join(QLatin1String("<")) + QLatin1Char('<')).toUtf8();
	//qDebug() << "Server caps ver: " << (prep.join(QLatin1String("<")) + QLatin1Char('<'))
	//         << "Hash:" << QString::fromLatin1(QCryptographicHash::hash(ba, algo).toBase64());
	return QString::fromLatin1(QCryptographicHash::hash(ba, algo).toBase64());
}

DiscoItem DiscoItem::fromDiscoInfoResult(const QDomElement &q)
{
	DiscoItem item;

	item.setNode( q.attribute("node") );

	QStringList features;
	DiscoItem::Identities identities;
	QList<XData> extList;

	for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
		QDomElement e = n.toElement();
		if( e.isNull() )
			continue;

		if ( e.tagName() == "feature" ) {
			features << e.attribute("var");
		}
		else if ( e.tagName() == "identity" ) {
			DiscoItem::Identity id;

			id.category = e.attribute("category");
			id.type     = e.attribute("type");
			id.lang     = e.attribute("lang");
			id.name     = e.attribute("name");

			identities.append( id );
		}
		else if (e.tagName() == QLatin1String("x") && e.attribute("xmlns") == QLatin1String("jabber:x:data")) {
			XData form;
			form.fromXml(e);
			extList.append(form);
		}
	}

	item.setFeatures( features );
	item.setIdentities( identities );
	item.setExtensions( extList );

	return item;
}

QDomElement DiscoItem::toDiscoInfoResult(QDomDocument *doc) const
{
	QDomElement q = doc->createElementNS(QLatin1String("http://jabber.org/protocol/disco#info"), QLatin1String("query"));
	q.setAttribute("node", d->node);

	foreach (const Identity &id, d->identities) {
		QDomElement idel = q.appendChild(doc->createElement(QLatin1String("identity"))).toElement();
		idel.setAttribute("category", id.category);
		idel.setAttribute("type", id.type);
		if (!id.lang.isEmpty()) {
			idel.setAttribute("lang", id.lang);
		}
		if (!id.name.isEmpty()) {
			idel.setAttribute("name", id.name);
		}
	}

	foreach (const QString &f, d->features.list()) {
		QDomElement fel = q.appendChild(doc->createElement(QLatin1String("feature"))).toElement();
		fel.setAttribute("var", f);
	}

	foreach (const XData &f, d->exts) {
		q.appendChild(f.toXml(doc));
	}

	return q;
}

const Jid &DiscoItem::jid() const
{
	return d->jid;
}

void DiscoItem::setJid(const Jid &j)
{
	d->jid = j;
}

const QString &DiscoItem::name() const
{
	return d->name;
}

void DiscoItem::setName(const QString &n)
{
	d->name = n;
}

const QString &DiscoItem::node() const
{
	return d->node;
}

void DiscoItem::setNode(const QString &n)
{
	d->node = n;
}

DiscoItem::Action DiscoItem::action() const
{
	return d->action;
}

void DiscoItem::setAction(Action a)
{
	d->action = a;
}

const Features &DiscoItem::features() const
{
	return d->features;
}

void DiscoItem::setFeatures(const Features &f)
{
	d->features = f;
}

const DiscoItem::Identities &DiscoItem::identities() const
{
	return d->identities;
}

void DiscoItem::setIdentities(const Identities &i)
{
	d->identities = i;

	if ( name().isEmpty() && i.count() )
		setName( i.first().name );
}

const QList<XData> &DiscoItem::extensions() const
{
	return d->exts;
}

void DiscoItem::setExtensions(const QList<XData> &extlist)
{
	d->exts = extlist;
}

XData DiscoItem::registeredExtension(const QString &ns) const
{
	foreach (const XData &xd, d->exts) {
		if (xd.registrarType() == ns) {
			return xd;
		}
	}
	return XData();
}

DiscoItem::Action DiscoItem::string2action(QString s)
{
	Action a;

	if ( s == "update" )
		a = Update;
	else if ( s == "remove" )
		a = Remove;
	else
		a = None;

	return a;
}

QString DiscoItem::action2string(Action a)
{
	QString s;

	if ( a == Update )
		s = "update";
	else if ( a == Remove )
		s = "remove";
	else
		s = QString::null;

	return s;
}



bool XMPP::operator<(const DiscoItem::Identity &a, const DiscoItem::Identity &b)
{
	int r = a.category.compare(b.category);
	if (!r) {
		r = a.type.compare(b.type);
		if (!r) {
			r = a.lang.compare(b.lang);
			if (!r) {
				r = a.name.compare(b.name);
			}
		}
	}

	return r < 0;
}

bool DiscoItem::Identity::operator==(const DiscoItem::Identity &other) const
{
	return category == other.category && type == other.type &&
	        lang == other.lang && name == other.name;
}
