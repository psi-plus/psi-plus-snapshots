/*
 * tasks.cpp - basic tasks
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

#include <QRegExp>
#include <QList>
#include <QTimer>

#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"
#include "xmpp_vcard.h"
#include "xmpp_bitsofbinary.h"
#include "xmpp/base/timezone.h"
#include "xmpp_caps.h"

using namespace XMPP;


static QString lineEncode(QString str)
{
	str.replace(QRegExp("\\\\"), "\\\\");   // backslash to double-backslash
	str.replace(QRegExp("\\|"), "\\p");     // pipe to \p
	str.replace(QRegExp("\n"), "\\n");      // newline to \n
	return str;
}

static QString lineDecode(const QString &str)
{
	QString ret;

	for(int n = 0; n < str.length(); ++n) {
		if(str.at(n) == '\\') {
			++n;
			if(n >= str.length())
				break;

			if(str.at(n) == 'n')
				ret.append('\n');
			if(str.at(n) == 'p')
				ret.append('|');
			if(str.at(n) == '\\')
				ret.append('\\');
		}
		else {
			ret.append(str.at(n));
		}
	}

	return ret;
}

static Roster xmlReadRoster(const QDomElement &q, bool push)
{
	Roster r;

	for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
		QDomElement i = n.toElement();
		if(i.isNull())
			continue;

		if(i.tagName() == "item") {
			RosterItem item;
			item.fromXml(i);

			if(push)
				item.setIsPush(true);

			r += item;
		}
	}

	return r;
}

//----------------------------------------------------------------------------
// JT_Session
//----------------------------------------------------------------------------
//
#include "protocol.h"

JT_Session::JT_Session(Task *parent) : Task(parent)
{
}

void JT_Session::onGo()
{
	QDomElement iq = createIQ(doc(), "set", "", id());
	QDomElement session = doc()->createElement("session");
	session.setAttribute("xmlns",NS_SESSION);
	iq.appendChild(session);
	send(iq);
}

bool JT_Session::take(const QDomElement& x)
{
	QString from = x.attribute("from");
	if (!from.endsWith("chat.facebook.com")) {
		// remove this code when chat.facebook.com is disabled completely
		from.clear();
	}
	if(!iqVerify(x, from, id()))
		return false;

	if(x.attribute("type") == "result") {
		setSuccess();
	}
	else {
		setError(x);
	}
	return true;
}

//----------------------------------------------------------------------------
// JT_Register
//----------------------------------------------------------------------------
class JT_Register::Private
{
public:
	Private() {}

	Form form;
	XData xdata;
	bool hasXData;
	Jid jid;
	int type;
};

JT_Register::JT_Register(Task *parent)
:Task(parent)
{
	d = new Private;
	d->type = -1;
	d->hasXData = false;
}

JT_Register::~JT_Register()
{
	delete d;
}

void JT_Register::reg(const QString &user, const QString &pass)
{
	d->type = 0;
	to = client()->host();
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:register");
	iq.appendChild(query);
	query.appendChild(textTag(doc(), "username", user));
	query.appendChild(textTag(doc(), "password", pass));
}

void JT_Register::changepw(const QString &pass)
{
	d->type = 1;
	to = client()->host();
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:register");
	iq.appendChild(query);
	query.appendChild(textTag(doc(), "username", client()->user()));
	query.appendChild(textTag(doc(), "password", pass));
}

void JT_Register::unreg(const Jid &j)
{
	d->type = 2;
	to = j.isEmpty() ? client()->host() : j.full();
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:register");
	iq.appendChild(query);

	// this may be useful
	if(!d->form.key().isEmpty())
		query.appendChild(textTag(doc(), "key", d->form.key()));

	query.appendChild(doc()->createElement("remove"));
}

void JT_Register::getForm(const Jid &j)
{
	d->type = 3;
	to = j;
	iq = createIQ(doc(), "get", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:register");
	iq.appendChild(query);
}

void JT_Register::setForm(const Form &form)
{
	d->type = 4;
	to = form.jid();
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:register");
	iq.appendChild(query);

	// key?
	if(!form.key().isEmpty())
		query.appendChild(textTag(doc(), "key", form.key()));

	// fields
	for(Form::ConstIterator it = form.begin(); it != form.end(); ++it) {
		const FormField &f = *it;
		query.appendChild(textTag(doc(), f.realName(), f.value()));
	}
}

void JT_Register::setForm(const Jid& to, const XData& xdata)
{
	d->type = 4;
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:register");
	iq.appendChild(query);
	query.appendChild(xdata.toXml(doc(), true));
}

const Form & JT_Register::form() const
{
	return d->form;
}

bool JT_Register::hasXData() const
{
	return d->hasXData;
}

const XData& JT_Register::xdata() const
{
	return d->xdata;
}

void JT_Register::onGo()
{
	send(iq);
}

bool JT_Register::take(const QDomElement &x)
{
	if(!iqVerify(x, to, id()))
		return false;

	Jid from(x.attribute("from"));
	if(x.attribute("type") == "result") {
		if(d->type == 3) {
			d->form.clear();
			d->form.setJid(from);

			QDomElement q = queryTag(x);
			for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
				QDomElement i = n.toElement();
				if(i.isNull())
					continue;

				if(i.tagName() == "instructions")
					d->form.setInstructions(tagContent(i));
				else if(i.tagName() == "key")
					d->form.setKey(tagContent(i));
				else if(i.tagName() == "x" && i.attribute("xmlns") == "jabber:x:data") {
					d->xdata.fromXml(i);
					d->hasXData = true;
				}
				else if(i.tagName() == "data" && i.attribute("xmlns") == "urn:xmpp:bob") {
					client()->bobManager()->append(BoBData(i)); // xep-0231
				}
				else {
					FormField f;
					if(f.setType(i.tagName())) {
						f.setValue(tagContent(i));
						d->form += f;
					}
				}
			}
		}

		setSuccess();
	}
	else
		setError(x);

	return true;
}

//----------------------------------------------------------------------------
// JT_UnRegister
//----------------------------------------------------------------------------
class JT_UnRegister::Private
{
public:
	Private() { }

	Jid j;
	JT_Register *jt_reg;
};

JT_UnRegister::JT_UnRegister(Task *parent)
: Task(parent)
{
	d = new Private;
	d->jt_reg = 0;
}

JT_UnRegister::~JT_UnRegister()
{
	delete d->jt_reg;
	delete d;
}

void JT_UnRegister::unreg(const Jid &j)
{
	d->j = j;
}

void JT_UnRegister::onGo()
{
	delete d->jt_reg;

	d->jt_reg = new JT_Register(this);
	d->jt_reg->getForm(d->j);
	connect(d->jt_reg, SIGNAL(finished()), SLOT(getFormFinished()));
	d->jt_reg->go(false);
}

void JT_UnRegister::getFormFinished()
{
	disconnect(d->jt_reg, 0, this, 0);

	d->jt_reg->unreg(d->j);
	connect(d->jt_reg, SIGNAL(finished()), SLOT(unregFinished()));
	d->jt_reg->go(false);
}

void JT_UnRegister::unregFinished()
{
	if ( d->jt_reg->success() )
		setSuccess();
	else
		setError(d->jt_reg->statusCode(), d->jt_reg->statusString());

	delete d->jt_reg;
	d->jt_reg = 0;
}

//----------------------------------------------------------------------------
// JT_Roster
//----------------------------------------------------------------------------
class JT_Roster::Private
{
public:
	Private() {}

	Roster roster;
	QString groupsDelimiter;
	QList<QDomElement> itemList;
};


JT_Roster::JT_Roster(Task *parent)
:Task(parent)
{
	type = -1;
	d = new Private;
}

JT_Roster::~JT_Roster()
{
	delete d;
}

void JT_Roster::get()
{
	type = 0;
	//to = client()->host();
	iq = createIQ(doc(), "get", to.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:roster");
	iq.appendChild(query);
}

void JT_Roster::set(const Jid &jid, const QString &name, const QStringList &groups)
{
	type = 1;
	//to = client()->host();
	QDomElement item = doc()->createElement("item");
	item.setAttribute("jid", jid.full());
	if(!name.isEmpty())
		item.setAttribute("name", name);
	for(QStringList::ConstIterator it = groups.begin(); it != groups.end(); ++it)
		item.appendChild(textTag(doc(), "group", *it));
	d->itemList += item;
}

void JT_Roster::remove(const Jid &jid)
{
	type = 2;
	//to = client()->host();
	QDomElement item = doc()->createElement("item");
	item.setAttribute("jid", jid.full());
	item.setAttribute("subscription", "remove");
	d->itemList += item;
}

void JT_Roster::getGroupsDelimiter()
{
	type = 3;
	//to = client()->host();
	iq = createIQ(doc(), "get", to.full(), id());

	QDomElement roster = doc()->createElement("roster");
	roster.setAttribute("xmlns", "roster:delimiter");

	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:private");
	query.appendChild(roster);

	iq.appendChild(query);
}

void JT_Roster::setGroupsDelimiter(const QString &groupsDelimiter)
{
	type = 4;
	//to = client()->host();
	iq = createIQ(doc(), "set", to.full(), id());

	QDomText text = doc()->createTextNode(groupsDelimiter);

	QDomElement roster = doc()->createElement("roster");
	roster.setAttribute("xmlns", "roster:delimiter");
	roster.appendChild(text);

	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:private");
	query.appendChild(roster);

	iq.appendChild(query);
}

void JT_Roster::onGo()
{
	if (type == 0) {
		send(iq);
	}
	else if(type == 1 || type == 2) {
		//to = client()->host();
		iq = createIQ(doc(), "set", to.full(), id());
		QDomElement query = doc()->createElement("query");
		query.setAttribute("xmlns", "jabber:iq:roster");
		iq.appendChild(query);
		foreach (const QDomElement& it, d->itemList)
			query.appendChild(it);
		send(iq);
	}
	else if (type == 3) {
		send(iq);
	}
	else if (type == 4) {
		send(iq);
	}
}

const Roster & JT_Roster::roster() const
{
	return d->roster;
}

QString JT_Roster::groupsDelimiter() const
{
	return d->groupsDelimiter;
}

QString JT_Roster::toString() const
{
	if(type != 1)
		return "";

	QDomElement i = doc()->createElement("request");
	i.setAttribute("type", "JT_Roster");
	foreach (const QDomElement& it, d->itemList)
		i.appendChild(it);
	return lineEncode(Stream::xmlToString(i));
	return "";
}

bool JT_Roster::fromString(const QString &str)
{
	QDomDocument *dd = new QDomDocument;
	if(!dd->setContent(lineDecode(str).toUtf8()))
		return false;
	QDomElement e = doc()->importNode(dd->documentElement(), true).toElement();
	delete dd;

	if(e.tagName() != "request" || e.attribute("type") != "JT_Roster")
		return false;

	type = 1;
	d->itemList.clear();
	for(QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
		QDomElement i = n.toElement();
		if(i.isNull())
			continue;
		d->itemList += i;
	}

	return true;
}

bool JT_Roster::take(const QDomElement &x)
{
	if(!iqVerify(x, client()->host(), id()))
		return false;

	// get
	if(type == 0) {
		if(x.attribute("type") == "result") {
			QDomElement q = queryTag(x);
			d->roster = xmlReadRoster(q, false);
			setSuccess();
		}
		else {
			setError(x);
		}

		return true;
	}
	// set
	else if(type == 1) {
		if(x.attribute("type") == "result")
			setSuccess();
		else
			setError(x);

		return true;
	}
	// remove
	else if(type == 2) {
		setSuccess();
		return true;
	}
	// getGroupsDelimiter
	else if (type == 3) {
		if(x.attribute("type") == "result") {
			QDomElement q = queryTag(x);
			QDomElement delimiter = q.firstChild().toElement();
			d->groupsDelimiter = delimiter.firstChild().toText().data();
			setSuccess();
		}
		else {
			setError(x);
		}
		return true;
	}
	// setGroupsDelimiter
	else if (type == 4) {
		setSuccess();
		return true;
	}

	return false;
}


//----------------------------------------------------------------------------
// JT_PushRoster
//----------------------------------------------------------------------------
JT_PushRoster::JT_PushRoster(Task *parent)
:Task(parent)
{
}

JT_PushRoster::~JT_PushRoster()
{
}

bool JT_PushRoster::take(const QDomElement &e)
{
	// must be an iq-set tag
	if(e.tagName() != "iq" || e.attribute("type") != "set")
		return false;

	if(!iqVerify(e, client()->host(), "", "jabber:iq:roster"))
		return false;

	roster(xmlReadRoster(queryTag(e), true));
	send(createIQ(doc(), "result", e.attribute("from"), e.attribute("id")));

	return true;
}


//----------------------------------------------------------------------------
// JT_Presence
//----------------------------------------------------------------------------
JT_Presence::JT_Presence(Task *parent)
:Task(parent)
{
	type = -1;
}

JT_Presence::~JT_Presence()
{
}

void JT_Presence::pres(const Status &s)
{
	type = 0;

	tag = doc()->createElement("presence");
	if(!s.isAvailable()) {
		tag.setAttribute("type", "unavailable");
		if(!s.status().isEmpty())
			tag.appendChild(textTag(doc(), "status", s.status()));
	}
	else {
		if(s.isInvisible())
			tag.setAttribute("type", "invisible");

		if(!s.show().isEmpty())
			tag.appendChild(textTag(doc(), "show", s.show()));
		if(!s.status().isEmpty())
			tag.appendChild(textTag(doc(), "status", s.status()));

		tag.appendChild( textTag(doc(), "priority", QString("%1").arg(s.priority()) ) );

		if(!s.keyID().isEmpty()) {
			QDomElement x = textTag(doc(), "x", s.keyID());
			x.setAttribute("xmlns", "http://jabber.org/protocol/e2e");
			tag.appendChild(x);
		}
		if(!s.xsigned().isEmpty()) {
			QDomElement x = textTag(doc(), "x", s.xsigned());
			x.setAttribute("xmlns", "jabber:x:signed");
			tag.appendChild(x);
		}

		if (client()->capsManager()->isEnabled()) {
			CapsSpec cs = client()->caps();
			if (cs.isValid()) {
				tag.appendChild(cs.toXml(doc()));
			}
		}

		if(s.isMUC()) {
			QDomElement m = doc()->createElement("x");
			m.setAttribute("xmlns","http://jabber.org/protocol/muc");
			if (!s.mucPassword().isEmpty()) {
				m.appendChild(textTag(doc(),"password",s.mucPassword()));
			}
			if (s.hasMUCHistory()) {
				QDomElement h = doc()->createElement("history");
				if (s.mucHistoryMaxChars() >= 0)
					h.setAttribute("maxchars",s.mucHistoryMaxChars());
				if (s.mucHistoryMaxStanzas() >= 0)
					h.setAttribute("maxstanzas",s.mucHistoryMaxStanzas());
				if (s.mucHistorySeconds() >= 0)
					h.setAttribute("seconds",s.mucHistorySeconds());
				if (!s.mucHistorySince().isNull())
					h.setAttribute("since", s.mucHistorySince().toUTC().addSecs(1).toString(Qt::ISODate));
				m.appendChild(h);
			}
			tag.appendChild(m);
		}

		if(s.hasPhotoHash()) {
			QDomElement m = doc()->createElement("x");
			m.setAttribute("xmlns", "vcard-temp:x:update");
			m.appendChild(textTag(doc(), "photo", s.photoHash()));
			tag.appendChild(m);
		}

		// bits of binary
		foreach(const BoBData &bd, s.bobDataList()) {
			tag.appendChild(bd.toXml(doc()));
		}
	}
}

void JT_Presence::pres(const Jid &to, const Status &s)
{
	pres(s);
	tag.setAttribute("to", to.full());
}

void JT_Presence::sub(const Jid &to, const QString &subType, const QString& nick)
{
	type = 1;

	tag = doc()->createElement("presence");
	tag.setAttribute("to", to.full());
	tag.setAttribute("type", subType);
	if (!nick.isEmpty()) {
		QDomElement nick_tag = textTag(doc(),"nick",nick);
		nick_tag.setAttribute("xmlns","http://jabber.org/protocol/nick");
		tag.appendChild(nick_tag);
	}
}

void JT_Presence::probe(const Jid &to)
{
	type = 2;

	tag = doc()->createElement("presence");
	tag.setAttribute("to", to.full());
	tag.setAttribute("type", "probe");
}

void JT_Presence::onGo()
{
	send(tag);
	setSuccess();
}


//----------------------------------------------------------------------------
// JT_PushPresence
//----------------------------------------------------------------------------
JT_PushPresence::JT_PushPresence(Task *parent)
:Task(parent)
{
}

JT_PushPresence::~JT_PushPresence()
{
}

bool JT_PushPresence::take(const QDomElement &e)
{
	if(e.tagName() != "presence")
		return false;

	Jid j(e.attribute("from"));
	Status p;

	if(e.hasAttribute("type")) {
		QString type = e.attribute("type");
		if(type == "unavailable") {
			p.setIsAvailable(false);
		}
		else if(type == "error") {
			QString str = "";
			int code = 0;
			getErrorFromElement(e, client()->stream().baseNS(), &code, &str);
			p.setError(code, str);
		}
		else if(type == "subscribe" || type == "subscribed" || type == "unsubscribe" || type == "unsubscribed") {
			QString nick;
			QDomElement tag = e.firstChildElement("nick");
			if (!tag.isNull() && tag.attribute("xmlns") == "http://jabber.org/protocol/nick") {
				nick = tagContent(tag);
			}
			subscription(j, type, nick);
			return true;
		}
	}

	QDomElement tag;

	tag = e.firstChildElement("status");
	if(!tag.isNull())
		p.setStatus(tagContent(tag));
	tag = e.firstChildElement("show");
	if(!tag.isNull())
		p.setShow(tagContent(tag));
	tag = e.firstChildElement("priority");
	if(!tag.isNull())
		p.setPriority(tagContent(tag).toInt());

	QDateTime stamp;

	for(QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
		QDomElement i = n.toElement();
		if(i.isNull())
			continue;

		if(i.tagName() == "x" && i.attribute("xmlns") == "jabber:x:delay") {
			if(i.hasAttribute("stamp") && !stamp.isValid()) {
				stamp = stamp2TS(i.attribute("stamp"));
			}
		}
		else if(i.tagName() == "delay" && i.attribute("xmlns") == "urn:xmpp:delay") {
			if(i.hasAttribute("stamp") && !stamp.isValid()) {
				stamp = QDateTime::fromString(i.attribute("stamp").left(19), Qt::ISODate);
			}
		}
		else if(i.tagName() == "x" && i.attribute("xmlns") == "gabber:x:music:info") {
			QDomElement t;
			QString title, state;

			t = i.firstChildElement("title");
			if(!t.isNull())
				title = tagContent(t);
			t = i.firstChildElement("state");
			if(!t.isNull())
				state = tagContent(t);

			if(!title.isEmpty() && state == "playing")
				p.setSongTitle(title);
		}
		else if(i.tagName() == "x" && i.attribute("xmlns") == "jabber:x:signed") {
			p.setXSigned(tagContent(i));
		}
		else if(i.tagName() == "x" && i.attribute("xmlns") == "http://jabber.org/protocol/e2e") {
			p.setKeyID(tagContent(i));
		}
 		else if(i.tagName() == "c" && i.attribute("xmlns") == NS_CAPS) {
			p.setCaps(CapsSpec::fromXml(i));
  		}
		else if(i.tagName() == "x" && i.attribute("xmlns") == "vcard-temp:x:update") {
			QDomElement t;
			t = i.firstChildElement("photo");
			if (!t.isNull())
				p.setPhotoHash(tagContent(t));
			else
				p.setPhotoHash("");
		}
		else if(i.tagName() == "x" && i.attribute("xmlns") == "http://jabber.org/protocol/muc#user") {
			for(QDomNode muc_n = i.firstChild(); !muc_n.isNull(); muc_n = muc_n.nextSibling()) {
				QDomElement muc_e = muc_n.toElement();
				if(muc_e.isNull())
					continue;

				if (muc_e.tagName() == "item")
					p.setMUCItem(MUCItem(muc_e));
				else if (muc_e.tagName() == "status")
					p.addMUCStatus(muc_e.attribute("code").toInt());
				else if (muc_e.tagName() == "destroy")
					p.setMUCDestroy(MUCDestroy(muc_e));
			}
		}
		else if (i.tagName() == "data" && i.attribute("xmlns") == "urn:xmpp:bob") {
			BoBData bd(i);
			client()->bobManager()->append(bd);
			p.addBoBData(bd);
		}
	}


	if (stamp.isValid()) {
		if (client()->manualTimeZoneOffset()) {
			stamp = stamp.addSecs(client()->timeZoneOffset() * 3600);
		} else {
			stamp.setTimeSpec(Qt::UTC);
			stamp = stamp.toLocalTime();
		}
		p.setTimeStamp(stamp);
	}

	presence(j, p);

	return true;
}


//----------------------------------------------------------------------------
// JT_Message
//----------------------------------------------------------------------------
static QDomElement oldStyleNS(const QDomElement &e)
{
	// find closest parent with a namespace
	QDomNode par = e.parentNode();
	while(!par.isNull() && par.namespaceURI().isNull())
		par = par.parentNode();
	bool noShowNS = false;
	if(!par.isNull() && par.namespaceURI() == e.namespaceURI())
		noShowNS = true;

	QDomElement i;
	int x;
	//if(noShowNS)
		i = e.ownerDocument().createElement(e.tagName());
	//else
	//	i = e.ownerDocument().createElementNS(e.namespaceURI(), e.tagName());

	// copy attributes
	QDomNamedNodeMap al = e.attributes();
	for(x = 0; x < al.count(); ++x)
		i.setAttributeNode(al.item(x).cloneNode().toAttr());

	if(!noShowNS)
		i.setAttribute("xmlns", e.namespaceURI());

	// copy children
	QDomNodeList nl = e.childNodes();
	for(x = 0; x < nl.count(); ++x) {
		QDomNode n = nl.item(x);
		if(n.isElement())
			i.appendChild(oldStyleNS(n.toElement()));
		else
			i.appendChild(n.cloneNode());
	}
	return i;
}

JT_Message::JT_Message(Task *parent, const Message &msg, bool want_notify)
:Task(parent)
{
	m = msg;
	if (m.id().isEmpty())
		m.setId(id());
	w_notify = want_notify;
}

JT_Message::~JT_Message()
{
}

void JT_Message::onGo()
{
	Stanza s = m.toStanza(&(client()->stream()));
	QDomElement e = oldStyleNS(s.element());
	send(e, w_notify);
	setSuccess();
}


//----------------------------------------------------------------------------
// JT_PushMessage
//----------------------------------------------------------------------------
JT_PushMessage::JT_PushMessage(Task *parent)
:Task(parent)
{
}

JT_PushMessage::~JT_PushMessage()
{
}

bool JT_PushMessage::take(const QDomElement &e)
{
	if(e.tagName() != "message")
		return false;

	Stanza s = client()->stream().createStanza(addCorrectNS(e));
	if(s.isNull()) {
		//printf("take: bad stanza??\n");
		return false;
	}

	Message m;
	if(!m.fromStanza(s, client()->manualTimeZoneOffset(), client()->timeZoneOffset())) {
		//printf("bad message\n");
		return false;
	}

	emit message(m);
	return true;
}


//----------------------------------------------------------------------------
// JT_GetServices
//----------------------------------------------------------------------------
JT_GetServices::JT_GetServices(Task *parent)
:Task(parent)
{
}

void JT_GetServices::get(const Jid &j)
{
	agentList.clear();

	jid = j;
	iq = createIQ(doc(), "get", jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:agents");
	iq.appendChild(query);
}

const AgentList & JT_GetServices::agents() const
{
	return agentList;
}

void JT_GetServices::onGo()
{
	send(iq);
}

bool JT_GetServices::take(const QDomElement &x)
{
	if(!iqVerify(x, jid, id()))
		return false;

	if(x.attribute("type") == "result") {
		QDomElement q = queryTag(x);

		// agents
		for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
			QDomElement i = n.toElement();
			if(i.isNull())
				continue;

			if(i.tagName() == "agent") {
				AgentItem a;

				a.setJid(Jid(i.attribute("jid")));

				QDomElement tag;

				tag = i.firstChildElement("name");
				if(!tag.isNull())
					a.setName(tagContent(tag));

				// determine which namespaces does item support
				QStringList ns;

				tag = i.firstChildElement("register");
				if(!tag.isNull())
					ns << "jabber:iq:register";
				tag = i.firstChildElement("search");
				if(!tag.isNull())
					ns << "jabber:iq:search";
				tag = i.firstChildElement("groupchat");
				if(!tag.isNull())
					ns << "jabber:iq:conference";
				tag = i.firstChildElement("transport");
				if(!tag.isNull())
					ns << "jabber:iq:gateway";

				a.setFeatures(ns);

				agentList += a;
			}
		}

		setSuccess(true);
	}
	else {
		setError(x);
	}

	return true;
}


//----------------------------------------------------------------------------
// JT_VCard
//----------------------------------------------------------------------------
class JT_VCard::Private
{
public:
	Private() {}

	QDomElement iq;
	Jid jid;
	VCard vcard;
};

JT_VCard::JT_VCard(Task *parent)
:Task(parent)
{
	type = -1;
	d = new Private;
}

JT_VCard::~JT_VCard()
{
	delete d;
}

void JT_VCard::get(const Jid &_jid)
{
	type = 0;
	d->jid = _jid;
	d->iq = createIQ(doc(), "get", type == 1 ? Jid().full() : d->jid.full(), id());
	QDomElement v = doc()->createElement("vCard");
	v.setAttribute("xmlns", "vcard-temp");
	d->iq.appendChild(v);
}

const Jid & JT_VCard::jid() const
{
	return d->jid;
}

const VCard & JT_VCard::vcard() const
{
	return d->vcard;
}

void JT_VCard::set(const VCard &card)
{
	type = 1;
	d->vcard = card;
	d->jid = "";
	d->iq = createIQ(doc(), "set", d->jid.full(), id());
	d->iq.appendChild(card.toXml(doc()) );
}

void JT_VCard::set(const Jid &j, const VCard &card)
{
	type = 1;
	d->vcard = card;
	d->jid = j;
	d->iq = createIQ(doc(), "set", "", id());
	d->iq.appendChild(card.toXml(doc()) );
}

void JT_VCard::onGo()
{
	send(d->iq);
}

bool JT_VCard::take(const QDomElement &x)
{
	Jid to = d->jid;
	if (to.bare() == client()->jid().bare())
		to = client()->host();
	if(!iqVerify(x, to, id()))
		return false;

	if(x.attribute("type") == "result") {
		if(type == 0) {
			for(QDomNode n = x.firstChild(); !n.isNull(); n = n.nextSibling()) {
				QDomElement q = n.toElement();
				if(q.isNull())
					continue;

				if(q.tagName().toUpper() == "VCARD") {
					if(d->vcard.fromXml(q)) {
						setSuccess();
						return true;
					}
				}
			}

			setError(ErrDisc + 1, tr("No VCard available"));
			return true;
		}
		else {
			setSuccess();
			return true;
		}
	}
	else {
		setError(x);
	}

	return true;
}


//----------------------------------------------------------------------------
// JT_Search
//----------------------------------------------------------------------------
class JT_Search::Private
{
public:
	Private() {}

	Jid jid;
	Form form;
	bool hasXData;
	XData xdata;
	QList<SearchResult> resultList;
};

JT_Search::JT_Search(Task *parent)
:Task(parent)
{
	d = new Private;
	type = -1;
}

JT_Search::~JT_Search()
{
	delete d;
}

void JT_Search::get(const Jid &jid)
{
	type = 0;
	d->jid = jid;
	d->hasXData = false;
	d->xdata = XData();
	iq = createIQ(doc(), "get", d->jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:search");
	iq.appendChild(query);
}

void JT_Search::set(const Form &form)
{
	type = 1;
	d->jid = form.jid();
	d->hasXData = false;
	d->xdata = XData();
	iq = createIQ(doc(), "set", d->jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:search");
	iq.appendChild(query);

	// key?
	if(!form.key().isEmpty())
		query.appendChild(textTag(doc(), "key", form.key()));

	// fields
	for(Form::ConstIterator it = form.begin(); it != form.end(); ++it) {
		const FormField &f = *it;
		query.appendChild(textTag(doc(), f.realName(), f.value()));
	}
}

void JT_Search::set(const Jid &jid, const XData &form)
{
	type = 1;
	d->jid = jid;
	d->hasXData = false;
	d->xdata = XData();
	iq = createIQ(doc(), "set", d->jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:search");
	iq.appendChild(query);
	query.appendChild(form.toXml(doc(), true));
}

const Form & JT_Search::form() const
{
	return d->form;
}

const QList<SearchResult> & JT_Search::results() const
{
	return d->resultList;
}

bool JT_Search::hasXData() const
{
	return d->hasXData;
}

const XData & JT_Search::xdata() const
{
	return d->xdata;
}

void JT_Search::onGo()
{
	send(iq);
}

bool JT_Search::take(const QDomElement &x)
{
	if(!iqVerify(x, d->jid, id()))
		return false;

	Jid from(x.attribute("from"));
	if(x.attribute("type") == "result") {
		if(type == 0) {
			d->form.clear();
			d->form.setJid(from);

			QDomElement q = queryTag(x);
			for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
				QDomElement i = n.toElement();
				if(i.isNull())
					continue;

				if(i.tagName() == "instructions")
					d->form.setInstructions(tagContent(i));
				else if(i.tagName() == "key")
					d->form.setKey(tagContent(i));
				else if(i.tagName() == "x" && i.attribute("xmlns") == "jabber:x:data") {
					d->xdata.fromXml(i);
					d->hasXData = true;
				}
				else {
					FormField f;
					if(f.setType(i.tagName())) {
						f.setValue(tagContent(i));
						d->form += f;
					}
				}
			}
		}
		else {
			d->resultList.clear();

			QDomElement q = queryTag(x);
			for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
				QDomElement i = n.toElement();
				if(i.isNull())
					continue;

				if(i.tagName() == "item") {
					SearchResult r(Jid(i.attribute("jid")));

					QDomElement tag;

					tag = i.firstChildElement("nick");
					if(!tag.isNull())
						r.setNick(tagContent(tag));
					tag = i.firstChildElement("first");
					if(!tag.isNull())
						r.setFirst(tagContent(tag));
					tag = i.firstChildElement("last");
					if(!tag.isNull())
						r.setLast(tagContent(tag));
					tag = i.firstChildElement("email");
					if(!tag.isNull())
						r.setEmail(tagContent(tag));

					d->resultList += r;
				}
				else if(i.tagName() == "x" && i.attribute("xmlns") == "jabber:x:data") {
					d->xdata.fromXml(i);
					d->hasXData = true;
				}
			}
		}
		setSuccess();
	}
	else {
		setError(x);
	}

	return true;
}


//----------------------------------------------------------------------------
// JT_ClientVersion
//----------------------------------------------------------------------------
JT_ClientVersion::JT_ClientVersion(Task *parent)
:Task(parent)
{
}

void JT_ClientVersion::get(const Jid &jid)
{
	j = jid;
	iq = createIQ(doc(), "get", j.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:version");
	iq.appendChild(query);
}

void JT_ClientVersion::onGo()
{
	send(iq);
}

bool JT_ClientVersion::take(const QDomElement &x)
{
	if(!iqVerify(x, j, id()))
		return false;

	if(x.attribute("type") == "result") {
		QDomElement q = queryTag(x);
		QDomElement tag;
		tag = q.firstChildElement("name");
		if(!tag.isNull())
			v_name = tagContent(tag);
		tag = q.firstChildElement("version");
		if(!tag.isNull())
			v_ver = tagContent(tag);
		tag = q.firstChildElement("os");
		if(!tag.isNull())
			v_os = tagContent(tag);

		setSuccess();
	}
	else {
		setError(x);
	}

	return true;
}

const Jid & JT_ClientVersion::jid() const
{
	return j;
}

const QString & JT_ClientVersion::name() const
{
	return v_name;
}

const QString & JT_ClientVersion::version() const
{
	return v_ver;
}

const QString & JT_ClientVersion::os() const
{
	return v_os;
}


//----------------------------------------------------------------------------
// JT_EntityTime
//----------------------------------------------------------------------------
JT_EntityTime::JT_EntityTime(Task* parent) : Task(parent)
{
}

/**
 * \brief Queried entity's JID.
 */
const Jid & JT_EntityTime::jid() const
{
	return j;
}

/**
 * \brief Prepares the task to get information from JID.
 */
void JT_EntityTime::get(const Jid &jid)
{
	j = jid;
	iq = createIQ(doc(), "get", jid.full(), id());
	QDomElement time = doc()->createElement("time");
	time.setAttribute("xmlns", "urn:xmpp:time");
	iq.appendChild(time);
}

void JT_EntityTime::onGo()
{
	send(iq);
}

bool JT_EntityTime::take(const QDomElement &x)
{
	if (!iqVerify(x, j, id()))
		return false;

	if (x.attribute("type") == "result") {
		QDomElement q = x.firstChildElement("time");
		QDomElement tag;
		tag = q.firstChildElement("utc");
		do {
			if (tag.isNull()) {
				break;
			}
			utc = QDateTime::fromString(tagContent(tag), Qt::ISODate);
			tag = q.firstChildElement("tzo");
			if (!utc.isValid() || tag.isNull()) {
				break;
			}
			tzo = TimeZone::tzdToInt(tagContent(tag));
			if (tzo == -1) {
				break;
			}
			setSuccess();
			return true;
		}
		while (false);
		setError(406);
	}
	else {
		setError(x);
	}

	return true;
}

const QDateTime & JT_EntityTime::dateTime() const
{
	return utc;
}

int JT_EntityTime::timezoneOffset() const
{
	return tzo;
}


//----------------------------------------------------------------------------
// JT_ServInfo
//----------------------------------------------------------------------------
JT_ServInfo::JT_ServInfo(Task *parent)
:Task(parent)
{
}

JT_ServInfo::~JT_ServInfo()
{
}

bool JT_ServInfo::take(const QDomElement &e)
{
	if(e.tagName() != "iq" || e.attribute("type") != "get")
		return false;

	QString ns = queryNS(e);
	if(ns == "jabber:iq:version") {
		QDomElement iq = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
		QDomElement query = doc()->createElement("query");
		query.setAttribute("xmlns", "jabber:iq:version");
		iq.appendChild(query);
		query.appendChild(textTag(doc(), "name", client()->clientName()));
		query.appendChild(textTag(doc(), "version", client()->clientVersion()));
		query.appendChild(textTag(doc(), "os", client()->OSName() + ' ' + client()->OSVersion()));
		send(iq);
		return true;
	}
	else if (ns == "urn:xmpp:time") {
		QDomElement iq = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
		QDomElement time = doc()->createElement("time");
		time.setAttribute("xmlns", ns);
		iq.appendChild(time);

		QDateTime local = QDateTime::currentDateTime();

		int off = TimeZone::offsetFromUtc();
		QTime t = QTime(0, 0).addSecs(qAbs(off)*60);
		QString tzo = (off < 0 ? "-" : "+") + t.toString("HH:mm");
		time.appendChild(textTag(doc(), "tzo", tzo));
		QString localTimeStr = local.toUTC().toString(Qt::ISODate);
		if (!localTimeStr.endsWith("Z"))
			localTimeStr.append("Z");
		time.appendChild(textTag(doc(), "utc", localTimeStr));

		send(iq);
		return true;
	}
	else if(ns == "http://jabber.org/protocol/disco#info") {
		// Find out the node
		QString node;
		QDomElement q = e.firstChildElement("query");
		if(!q.isNull()) // NOTE: Should always be true, since a NS was found above
			node = q.attribute("node");

		if (node.isEmpty() || node == client()->caps().flatten()) {

			QDomElement iq = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
			DiscoItem item = client()->makeDiscoResult(node);
			iq.appendChild(item.toDiscoInfoResult(doc()));
			send(iq);
		}
		else {
			// Create error reply
			QDomElement error_reply = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));

			// Copy children
			for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
				error_reply.appendChild(n.cloneNode());
			}

			// Add error
			QDomElement error = doc()->createElement("error");
			error.setAttribute("type","cancel");
			error_reply.appendChild(error);
			QDomElement error_type = doc()->createElement("item-not-found");
			error_type.setAttribute("xmlns","urn:ietf:params:xml:ns:xmpp-stanzas");
			error.appendChild(error_type);
			send(error_reply);
		}
		return true;
	}

	return false;
}


//----------------------------------------------------------------------------
// JT_Gateway
//----------------------------------------------------------------------------
JT_Gateway::JT_Gateway(Task *parent)
:Task(parent)
{
	type = -1;
}

void JT_Gateway::get(const Jid &jid)
{
	type = 0;
	v_jid = jid;
	iq = createIQ(doc(), "get", v_jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:gateway");
	iq.appendChild(query);
}

void JT_Gateway::set(const Jid &jid, const QString &prompt)
{
	type = 1;
	v_jid = jid;
	v_prompt = prompt;
	iq = createIQ(doc(), "set", v_jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "jabber:iq:gateway");
	iq.appendChild(query);
	query.appendChild(textTag(doc(), "prompt", v_prompt));
}

void JT_Gateway::onGo()
{
	send(iq);
}

Jid JT_Gateway::jid() const
{
	return v_jid;
}

QString JT_Gateway::desc() const
{
	return v_desc;
}

QString JT_Gateway::prompt() const
{
	return v_prompt;
}

Jid JT_Gateway::translatedJid() const
{
	return v_translatedJid;
}

bool JT_Gateway::take(const QDomElement &x)
{
	if(!iqVerify(x, v_jid, id()))
		return false;

	if(x.attribute("type") == "result") {
		if(type == 0) {
			QDomElement query = queryTag(x);
			QDomElement tag;
			tag = query.firstChildElement("desc");
			if (!tag.isNull()) {
				v_desc = tagContent(tag);
			}
			tag = query.firstChildElement("prompt");
			if (!tag.isNull()) {
				v_prompt = tagContent(tag);
			}
		}
		else {
			QDomElement query = queryTag(x);
			QDomElement tag;
			tag = query.firstChildElement("jid");
			if (!tag.isNull()) {
				v_translatedJid = tagContent(tag);
			}
			// we used to read 'prompt' in the past
			// and some gateways still send it
			tag = query.firstChildElement("prompt");
			if (!tag.isNull()) {
				v_prompt = tagContent(tag);
			}
		}

		setSuccess();
	}
	else {
		setError(x);
	}

	return true;
}

//----------------------------------------------------------------------------
// JT_Browse
//----------------------------------------------------------------------------
class JT_Browse::Private
{
public:
	QDomElement iq;
	Jid jid;
	AgentList agentList;
	AgentItem root;
};

JT_Browse::JT_Browse (Task *parent)
:Task (parent)
{
	d = new Private;
}

JT_Browse::~JT_Browse ()
{
	delete d;
}

void JT_Browse::get (const Jid &j)
{
	d->agentList.clear();

	d->jid = j;
	d->iq = createIQ(doc(), "get", d->jid.full(), id());
	QDomElement query = doc()->createElement("item");
	query.setAttribute("xmlns", "jabber:iq:browse");
	d->iq.appendChild(query);
}

const AgentList & JT_Browse::agents() const
{
	return d->agentList;
}

const AgentItem & JT_Browse::root() const
{
	return d->root;
}

void JT_Browse::onGo ()
{
	send(d->iq);
}

AgentItem JT_Browse::browseHelper (const QDomElement &i)
{
	AgentItem a;

	if ( i.tagName() == "ns" )
		return a;

	a.setName ( i.attribute("name") );
	a.setJid  ( i.attribute("jid") );

	// there are two types of category/type specification:
	//
	//   1. <item category="category_name" type="type_name" />
	//   2. <category_name type="type_name" />

	if ( i.tagName() == "item" || i.tagName() == "query" )
		a.setCategory ( i.attribute("category") );
	else
		a.setCategory ( i.tagName() );

	a.setType ( i.attribute("type") );

	QStringList ns;
	for(QDomNode n = i.firstChild(); !n.isNull(); n = n.nextSibling()) {
		QDomElement i = n.toElement();
		if(i.isNull())
			continue;

		if ( i.tagName() == "ns" )
			ns << i.text();
	}

	// For now, conference.jabber.org returns proper namespace only
	// when browsing individual rooms. So it's a quick client-side fix.
	if ( !a.features().canGroupchat() && a.category() == "conference" )
		ns << "jabber:iq:conference";

	a.setFeatures (ns);

	return a;
}

bool JT_Browse::take(const QDomElement &x)
{
	if(!iqVerify(x, d->jid, id()))
		return false;

	if(x.attribute("type") == "result") {
		for(QDomNode n = x.firstChild(); !n.isNull(); n = n.nextSibling()) {
			QDomElement i = n.toElement();
			if(i.isNull())
				continue;

			d->root = browseHelper (i);

			for(QDomNode nn = i.firstChild(); !nn.isNull(); nn = nn.nextSibling()) {
				QDomElement e = nn.toElement();
				if ( e.isNull() )
					continue;
				if ( e.tagName() == "ns" )
					continue;

				d->agentList += browseHelper (e);
			}
		}

		setSuccess(true);
	}
	else {
		setError(x);
	}

	return true;
}

//----------------------------------------------------------------------------
// JT_DiscoItems
//----------------------------------------------------------------------------
class JT_DiscoItems::Private
{
public:
	Private() { }

	QDomElement iq;
	Jid jid;
	DiscoList items;
};

JT_DiscoItems::JT_DiscoItems(Task *parent)
: Task(parent)
{
	d = new Private;
}

JT_DiscoItems::~JT_DiscoItems()
{
	delete d;
}

void JT_DiscoItems::get(const DiscoItem &item)
{
	get(item.jid(), item.node());
}

void JT_DiscoItems::get (const Jid &j, const QString &node)
{
	d->items.clear();

	d->jid = j;
	d->iq = createIQ(doc(), "get", d->jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "http://jabber.org/protocol/disco#items");

	if ( !node.isEmpty() )
		query.setAttribute("node", node);

	d->iq.appendChild(query);
}

const DiscoList &JT_DiscoItems::items() const
{
	return d->items;
}

void JT_DiscoItems::onGo ()
{
	send(d->iq);
}

bool JT_DiscoItems::take(const QDomElement &x)
{
	if(!iqVerify(x, d->jid, id()))
		return false;

	if(x.attribute("type") == "result") {
		QDomElement q = queryTag(x);

		for(QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
			QDomElement e = n.toElement();
			if( e.isNull() )
				continue;

			if ( e.tagName() == "item" ) {
				DiscoItem item;

				item.setJid ( e.attribute("jid")  );
				item.setName( e.attribute("name") );
				item.setNode( e.attribute("node") );
				item.setAction( DiscoItem::string2action(e.attribute("action")) );

				d->items.append( item );
			}
		}

		setSuccess(true);
	}
	else {
		setError(x);
	}

	return true;
}

//----------------------------------------------------------------------------
// JT_DiscoPublish
//----------------------------------------------------------------------------
class JT_DiscoPublish::Private
{
public:
	Private() { }

	QDomElement iq;
	Jid jid;
	DiscoList list;
};

JT_DiscoPublish::JT_DiscoPublish(Task *parent)
: Task(parent)
{
	d = new Private;
}

JT_DiscoPublish::~JT_DiscoPublish()
{
	delete d;
}

void JT_DiscoPublish::set(const Jid &j, const DiscoList &list)
{
	d->list = list;
	d->jid = j;

	d->iq = createIQ(doc(), "set", d->jid.full(), id());
	QDomElement query = doc()->createElement("query");
	query.setAttribute("xmlns", "http://jabber.org/protocol/disco#items");

	// FIXME: unsure about this
	//if ( !node.isEmpty() )
	//	query.setAttribute("node", node);

	DiscoList::ConstIterator it = list.begin();
	for ( ; it != list.end(); ++it) {
		QDomElement w = doc()->createElement("item");

		w.setAttribute("jid", (*it).jid().full());
		if ( !(*it).name().isEmpty() )
			w.setAttribute("name", (*it).name());
		if ( !(*it).node().isEmpty() )
		w.setAttribute("node", (*it).node());
		w.setAttribute("action", DiscoItem::action2string((*it).action()));

		query.appendChild( w );
	}

	d->iq.appendChild(query);
}

void JT_DiscoPublish::onGo ()
{
	send(d->iq);
}

bool JT_DiscoPublish::take(const QDomElement &x)
{
	if(!iqVerify(x, d->jid, id()))
		return false;

	if(x.attribute("type") == "result") {
		setSuccess(true);
	}
	else {
		setError(x);
	}

	return true;
}


// ---------------------------------------------------------
// JT_BoBServer
// ---------------------------------------------------------
JT_BoBServer::JT_BoBServer(Task *parent)
	: Task(parent)
{

}

bool JT_BoBServer::take(const QDomElement &e)
{
	if (e.tagName() != "iq" || e.attribute("type") != "get")
		return false;

	QDomElement data = e.firstChildElement("data");
	if (data.attribute("xmlns") == "urn:xmpp:bob") {
		QDomElement iq;
		BoBData bd = client()->bobManager()->bobData(data.attribute("cid"));
		if (bd.isNull()) {
			iq = createIQ(client()->doc(), "error",
						  e.attribute("from"), e.attribute("id"));
			Stanza::Error error(Stanza::Error::Cancel,
								Stanza::Error::ItemNotFound);
			iq.appendChild(error.toXml(*doc(), client()->stream().baseNS()));
		}
		else {
			iq = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
			iq.appendChild(bd.toXml(doc()));
		}
		send(iq);
		return true;
	}
	return false;
}


//----------------------------------------------------------------------------
// JT_BitsOfBinary
//----------------------------------------------------------------------------
class JT_BitsOfBinary::Private
{
public:
	Private() { }

	QDomElement iq;
	Jid jid;
	QString cid;
	BoBData data;
};

JT_BitsOfBinary::JT_BitsOfBinary(Task *parent)
: Task(parent)
{
	d = new Private;
}

JT_BitsOfBinary::~JT_BitsOfBinary()
{
	delete d;
}

void JT_BitsOfBinary::get(const Jid &j, const QString &cid)
{
	d->jid = j;
	d->cid = cid;

	d->data = client()->bobManager()->bobData(cid);
	if (d->data.isNull()) {
		d->iq = createIQ(doc(), "get", d->jid.full(), id());
		QDomElement data = doc()->createElement("data");
		data.setAttribute("xmlns", "urn:xmpp:bob");
		data.setAttribute("cid", cid);
		d->iq.appendChild(data);
	}
}

void JT_BitsOfBinary::onGo()
{
	if (d->data.isNull()) {
		send(d->iq);
	}
	else {
		setSuccess();
	}
}

bool JT_BitsOfBinary::take(const QDomElement &x)
{
	if (!iqVerify(x, d->jid, id())) {
		return false;
	}

	if (x.attribute("type") == "result") {
		QDomElement data = x.firstChildElement("data");

		if (!data.isNull() && data.attribute("cid") == d->cid) { // check xmlns?
			d->data.fromXml(data);
			client()->bobManager()->append(d->data);
		}

		setSuccess();
	}
	else {
		setError(x);
	}

	return true;
}

BoBData JT_BitsOfBinary::data()
{
	return d->data;
}

//----------------------------------------------------------------------------
// JT_PongServer
//----------------------------------------------------------------------------
/**
 * \class JT_PongServer
 * \brief Answers XMPP Pings
 */

JT_PongServer::JT_PongServer(Task *parent)
:Task(parent)
{

}

bool JT_PongServer::take(const QDomElement &e)
{
	if (e.tagName() != "iq" || e.attribute("type") != "get")
		return false;

	QDomElement ping = e.firstChildElement("ping");
	if (!e.isNull() && ping.attribute("xmlns") == "urn:xmpp:ping") {
		QDomElement iq = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
		send(iq);
		return true;
	}
	return false;
}
