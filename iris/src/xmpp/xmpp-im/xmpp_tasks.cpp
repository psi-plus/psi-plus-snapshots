/*
 * tasks.cpp - basic tasks
 * Copyright (C) 2001-2002  Justin Karneges
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

#include "xmpp_tasks.h"

#include "xmpp/base/timezone.h"
#include "xmpp_bitsofbinary.h"
#include "xmpp_caps.h"
#include "xmpp_captcha.h"
#include "xmpp_client.h"
#include "xmpp_roster.h"
#include "xmpp_vcard.h"
#include "xmpp_xmlcommon.h"

#include <QList>
#include <QRegularExpression>
#include <QTimer>

using namespace XMPP;

static QString lineEncode(QString str)
{
    static QRegularExpression backslash("\\\\");
    static QRegularExpression pipe("\\|");
    static QRegularExpression newline("\n");
    str.replace(backslash, "\\\\"); // backslash to double-backslash
    str.replace(pipe, "\\p");       // pipe to \p
    str.replace(newline, "\\n");    // newline to \n
    return str;
}

static QString lineDecode(const QString &str)
{
    QString ret;

    for (int n = 0; n < str.length(); ++n) {
        if (str.at(n) == '\\') {
            ++n;
            if (n >= str.length())
                break;

            if (str.at(n) == 'n')
                ret.append('\n');
            if (str.at(n) == 'p')
                ret.append('|');
            if (str.at(n) == '\\')
                ret.append('\\');
        } else {
            ret.append(str.at(n));
        }
    }

    return ret;
}

static Roster xmlReadRoster(const QDomElement &q, bool push)
{
    Roster r;

    for (QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
        QDomElement i = n.toElement();
        if (i.isNull())
            continue;

        if (i.tagName() == "item") {
            RosterItem item;
            item.fromXml(i);

            if (push)
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
#include "xmpp/xmpp-core/protocol.h"

JT_Session::JT_Session(Task *parent) : Task(parent) { }

void JT_Session::onGo()
{
    QDomElement iq      = createIQ(doc(), "set", "", id());
    QDomElement session = doc()->createElementNS(NS_SESSION, "session");
    iq.appendChild(session);
    send(iq);
}

bool JT_Session::take(const QDomElement &x)
{
    QString from = x.attribute("from");
    if (!from.endsWith("chat.facebook.com")) {
        // remove this code when chat.facebook.com is disabled completely
        from.clear();
    }
    if (!iqVerify(x, from, id()))
        return false;

    if (x.attribute("type") == "result") {
        setSuccess();
    } else {
        setError(x);
    }
    return true;
}

//----------------------------------------------------------------------------
// JT_Register
//----------------------------------------------------------------------------
class JT_Register::Private {
public:
    Private() = default;

    Form  form;
    XData xdata;
    bool  hasXData;
    bool  registered;
    Jid   jid;
    int   type;
};

JT_Register::JT_Register(Task *parent) : Task(parent)
{
    d             = new Private;
    d->type       = -1;
    d->hasXData   = false;
    d->registered = false;
}

JT_Register::~JT_Register() { delete d; }

void JT_Register::reg(const QString &user, const QString &pass)
{
    d->type           = 0;
    to                = client()->host();
    iq                = createIQ(doc(), "set", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:register", "query");
    iq.appendChild(query);
    query.appendChild(textTag(doc(), "username", user));
    query.appendChild(textTag(doc(), "password", pass));
}

void JT_Register::changepw(const QString &pass)
{
    d->type           = 1;
    to                = client()->host();
    iq                = createIQ(doc(), "set", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:register", "query");
    iq.appendChild(query);
    query.appendChild(textTag(doc(), "username", client()->user()));
    query.appendChild(textTag(doc(), "password", pass));
}

void JT_Register::unreg(const Jid &j)
{
    d->type           = 2;
    to                = j.isEmpty() ? client()->host() : j.full();
    iq                = createIQ(doc(), "set", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:register", "query");
    iq.appendChild(query);

    // this may be useful
    if (!d->form.key().isEmpty())
        query.appendChild(textTag(doc(), "key", d->form.key()));

    query.appendChild(doc()->createElement("remove"));
}

void JT_Register::getForm(const Jid &j)
{
    d->type           = 3;
    to                = j;
    iq                = createIQ(doc(), "get", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:register", "query");
    iq.appendChild(query);
}

void JT_Register::setForm(const Form &form)
{
    d->type           = 4;
    to                = form.jid();
    iq                = createIQ(doc(), "set", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:register", "query");
    iq.appendChild(query);

    // key?
    if (!form.key().isEmpty())
        query.appendChild(textTag(doc(), "key", form.key()));

    // fields
    for (const auto &f : form) {
        query.appendChild(textTag(doc(), f.realName(), f.value()));
    }
}

void JT_Register::setForm(const Jid &to, const XData &xdata)
{
    d->type           = 4;
    iq                = createIQ(doc(), "set", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:register", "query");
    iq.appendChild(query);
    query.appendChild(xdata.toXml(doc(), true));
}

const Form &JT_Register::form() const { return d->form; }

bool JT_Register::hasXData() const { return d->hasXData; }

const XData &JT_Register::xdata() const { return d->xdata; }

bool JT_Register::isRegistered() const { return d->registered; }

void JT_Register::onGo() { send(iq); }

bool JT_Register::take(const QDomElement &x)
{
    if (!iqVerify(x, to, id()))
        return false;

    Jid from(x.attribute("from"));
    if (x.attribute("type") == "result") {
        if (d->type == 3) {
            d->form.clear();
            d->form.setJid(from);

            QDomElement q = queryTag(x);
            for (QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
                QDomElement i = n.toElement();
                if (i.isNull())
                    continue;

                if (i.tagName() == "instructions")
                    d->form.setInstructions(tagContent(i));
                else if (i.tagName() == "key")
                    d->form.setKey(tagContent(i));
                else if (i.tagName() == QLatin1String("registered"))
                    d->registered = true;
                else if (i.tagName() == "x" && i.namespaceURI() == "jabber:x:data") {
                    d->xdata.fromXml(i);
                    d->hasXData = true;
                } else if (i.tagName() == "data" && i.namespaceURI() == "urn:xmpp:bob") {
                    client()->bobManager()->append(BoBData(i)); // xep-0231
                } else {
                    FormField f;
                    if (f.setType(i.tagName())) {
                        f.setValue(tagContent(i));
                        d->form += f;
                    }
                }
            }
        }

        setSuccess();
    } else
        setError(x);

    return true;
}

//----------------------------------------------------------------------------
// JT_UnRegister
//----------------------------------------------------------------------------
class JT_UnRegister::Private {
public:
    Private() = default;

    Jid          j;
    JT_Register *jt_reg = nullptr;
};

JT_UnRegister::JT_UnRegister(Task *parent) : Task(parent)
{
    d         = new Private;
    d->jt_reg = nullptr;
}

JT_UnRegister::~JT_UnRegister()
{
    delete d->jt_reg;
    delete d;
}

void JT_UnRegister::unreg(const Jid &j) { d->j = j; }

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
    disconnect(d->jt_reg, nullptr, this, nullptr);
    if (d->jt_reg->isRegistered()) {
        d->jt_reg->unreg(d->j);
        connect(d->jt_reg, SIGNAL(finished()), SLOT(unregFinished()));
        d->jt_reg->go(false);
    } else {
        setSuccess(); // no need to unregister
    }
}

void JT_UnRegister::unregFinished()
{
    if (d->jt_reg->success())
        setSuccess();
    else
        setError(d->jt_reg->statusCode(), d->jt_reg->statusString());

    delete d->jt_reg;
    d->jt_reg = nullptr;
}

//----------------------------------------------------------------------------
// JT_Roster
//----------------------------------------------------------------------------
class JT_Roster::Private {
public:
    Private() = default;

    Roster             roster;
    QString            groupsDelimiter;
    QList<QDomElement> itemList;
};

JT_Roster::JT_Roster(Task *parent) : Task(parent)
{
    type = Unknown;
    d    = new Private;
}

JT_Roster::~JT_Roster() { delete d; }

void JT_Roster::get()
{
    type = Get;
    // to = client()->host();
    iq                = createIQ(doc(), "get", to.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:roster", "query");
    iq.appendChild(query);
}

void JT_Roster::set(const Jid &jid, const QString &name, const QStringList &groups)
{
    type = Set;
    // to = client()->host();
    QDomElement item = doc()->createElement("item");
    item.setAttribute("jid", jid.full());
    if (!name.isEmpty())
        item.setAttribute("name", name);
    for (const auto &group : groups)
        item.appendChild(textTag(doc(), "group", group));
    d->itemList += item;
}

void JT_Roster::remove(const Jid &jid)
{
    type = Remove;
    // to = client()->host();
    QDomElement item = doc()->createElement("item");
    item.setAttribute("jid", jid.full());
    item.setAttribute("subscription", "remove");
    d->itemList += item;
}

void JT_Roster::getGroupsDelimiter()
{
    type = GetDelimiter;
    // to = client()->host();
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
    type = SetDelimiter;
    // to = client()->host();
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
    if (type == Get)
        send(iq);
    else if (type == Set || type == Remove) {
        // to = client()->host();
        iq                = createIQ(doc(), "set", to.full(), id());
        QDomElement query = doc()->createElementNS("jabber:iq:roster", "query");
        iq.appendChild(query);
        for (const QDomElement &it : std::as_const(d->itemList))
            query.appendChild(it);
        send(iq);
    } else if (type == GetDelimiter) {
        send(iq);
    } else if (type == SetDelimiter) {
        send(iq);
    }
}

const Roster &JT_Roster::roster() const { return d->roster; }

QString JT_Roster::groupsDelimiter() const { return d->groupsDelimiter; }

QString JT_Roster::toString() const
{
    if (type != Set)
        return "";

    QDomElement i = doc()->createElement("request");
    i.setAttribute("type", "JT_Roster");
    for (const QDomElement &it : std::as_const(d->itemList))
        i.appendChild(it);
    return lineEncode(Stream::xmlToString(i));
}

bool JT_Roster::fromString(const QString &str)
{
    QDomDocument *dd = new QDomDocument;
    if (!dd->setContent(lineDecode(str).toUtf8()))
        return false;
    QDomElement e = doc()->importNode(dd->documentElement(), true).toElement();
    delete dd;

    if (e.tagName() != "request" || e.attribute("type") != "JT_Roster")
        return false;

    type = Set;
    d->itemList.clear();
    for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
        QDomElement i = n.toElement();
        if (i.isNull())
            continue;
        d->itemList += i;
    }

    return true;
}

bool JT_Roster::take(const QDomElement &x)
{
    if (!iqVerify(x, client()->host(), id()))
        return false;

    if (type == Get) {
        if (x.attribute("type") == "result") {
            QDomElement q = queryTag(x);
            d->roster     = xmlReadRoster(q, false);
            setSuccess();
        } else {
            setError(x);
        }

        return true;
    } else if (type == Set) {
        if (x.attribute("type") == "result")
            setSuccess();
        else
            setError(x);

        return true;
    } else if (type == Remove) {
        setSuccess();
        return true;
    } else if (type == GetDelimiter) {
        if (x.attribute("type") == "result") {
            QDomElement q         = queryTag(x);
            QDomElement delimiter = q.firstChild().toElement();
            d->groupsDelimiter    = delimiter.firstChild().toText().data();
            setSuccess();
        } else {
            setError(x);
        }
        return true;
    } else if (type == SetDelimiter) {
        setSuccess();
        return true;
    }

    return false;
}

//----------------------------------------------------------------------------
// JT_PushRoster
//----------------------------------------------------------------------------
JT_PushRoster::JT_PushRoster(Task *parent) : Task(parent) { }

JT_PushRoster::~JT_PushRoster() { }

bool JT_PushRoster::take(const QDomElement &e)
{
    // must be an iq-set tag
    if (e.tagName() != "iq" || e.attribute("type") != "set")
        return false;

    if (!iqVerify(e, client()->host(), "", "jabber:iq:roster"))
        return false;

    emit roster(xmlReadRoster(queryTag(e), true));
    send(createIQ(doc(), "result", e.attribute("from"), e.attribute("id")));

    return true;
}

//----------------------------------------------------------------------------
// JT_Presence
//----------------------------------------------------------------------------
JT_Presence::JT_Presence(Task *parent) : Task(parent) { }

JT_Presence::~JT_Presence() { }

void JT_Presence::pres(const Status &s)
{
    type = 0;

    tag = doc()->createElement("presence");
    if (!s.isAvailable()) {
        tag.setAttribute("type", "unavailable");
        if (!s.status().isEmpty())
            tag.appendChild(textTag(doc(), "status", s.status()));
    } else {
        if (s.isInvisible())
            tag.setAttribute("type", "invisible");

        if (!s.show().isEmpty())
            tag.appendChild(textTag(doc(), "show", s.show()));
        if (!s.status().isEmpty())
            tag.appendChild(textTag(doc(), "status", s.status()));

        tag.appendChild(textTag(doc(), "priority", QString("%1").arg(s.priority())));

        if (!s.keyID().isEmpty()) {
            QDomElement x = textTagNS(doc(), "http://jabber.org/protocol/e2e", "x", s.keyID());
            tag.appendChild(x);
        }
        if (!s.xsigned().isEmpty()) {
            QDomElement x = textTagNS(doc(), "jabber:x:signed", "x", s.xsigned());
            tag.appendChild(x);
        }

        if (client()->capsManager()->isEnabled() && !client()->capsOptimizationAllowed()) {
            CapsSpec cs = client()->caps();
            if (cs.isValid()) {
                tag.appendChild(cs.toXml(doc()));
            }
        }

        if (s.isMUC()) {
            QDomElement m = doc()->createElementNS("http://jabber.org/protocol/muc", "x");
            if (!s.mucPassword().isEmpty()) {
                m.appendChild(textTag(doc(), "password", s.mucPassword()));
            }
            if (s.hasMUCHistory()) {
                QDomElement h = doc()->createElement("history");
                if (s.mucHistoryMaxChars() >= 0)
                    h.setAttribute("maxchars", s.mucHistoryMaxChars());
                if (s.mucHistoryMaxStanzas() >= 0)
                    h.setAttribute("maxstanzas", s.mucHistoryMaxStanzas());
                if (s.mucHistorySeconds() >= 0)
                    h.setAttribute("seconds", s.mucHistorySeconds());
                if (!s.mucHistorySince().isNull())
                    h.setAttribute("since", s.mucHistorySince().toUTC().addSecs(1).toString(Qt::ISODate));
                m.appendChild(h);
            }
            tag.appendChild(m);
        }

        if (s.hasPhotoHash()) {
            QDomElement m = doc()->createElementNS("vcard-temp:x:update", "x");
            m.appendChild(textTag(doc(), "photo", QString::fromLatin1(s.photoHash().toHex())));
            tag.appendChild(m);
        }

        // bits of binary
        const auto &bdlist = s.bobDataList();
        for (const BoBData &bd : bdlist) {
            tag.appendChild(bd.toXml(doc()));
        }
    }
}

void JT_Presence::pres(const Jid &to, const Status &s)
{
    pres(s);
    tag.setAttribute("to", to.full());
}

void JT_Presence::sub(const Jid &to, const QString &subType, const QString &nick)
{
    type = 1;

    tag = doc()->createElement("presence");
    tag.setAttribute("to", to.full());
    tag.setAttribute("type", subType);
    if (!nick.isEmpty()
        && (subType == QLatin1String("subscribe") || subType == QLatin1String("subscribed")
            || subType == QLatin1String("unsubscribe") || subType == QLatin1String("unsubscribed"))) {
        QDomElement nick_tag = textTagNS(doc(), "http://jabber.org/protocol/nick", "nick", nick);
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
JT_PushPresence::JT_PushPresence(Task *parent) : Task(parent) { }

JT_PushPresence::~JT_PushPresence() { }

bool JT_PushPresence::take(const QDomElement &e)
{
    if (e.tagName() != "presence")
        return false;

    Jid    j(e.attribute("from"));
    Status p;

    if (e.hasAttribute("type")) {
        QString type = e.attribute("type");
        if (type == "unavailable") {
            p.setIsAvailable(false);
        } else if (type == "error") {
            QString str  = "";
            int     code = 0;
            getErrorFromElement(e, client()->stream().baseNS(), &code, &str);
            p.setError(code, str);
        } else if (type == QLatin1String("subscribe") || type == QLatin1String("subscribed")
                   || type == QLatin1String("unsubscribe") || type == QLatin1String("unsubscribed")) {
            QString     nick;
            QDomElement tag = e.firstChildElement("nick");
            if (!tag.isNull() && tag.namespaceURI() == "http://jabber.org/protocol/nick") {
                nick = tagContent(tag);
            }
            emit subscription(j, type, nick);
            return true;
        }
    }

    QDomElement tag;

    tag = e.firstChildElement("status");
    if (!tag.isNull())
        p.setStatus(tagContent(tag));
    tag = e.firstChildElement("show");
    if (!tag.isNull())
        p.setShow(tagContent(tag));
    tag = e.firstChildElement("priority");
    if (!tag.isNull())
        p.setPriority(tagContent(tag).toInt());

    QDateTime stamp;

    for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
        QDomElement i = n.toElement();
        if (i.isNull())
            continue;

        if (i.tagName() == "x" && i.namespaceURI() == "jabber:x:delay") {
            if (i.hasAttribute("stamp") && !stamp.isValid()) {
                stamp = stamp2TS(i.attribute("stamp"));
            }
        } else if (i.tagName() == "delay" && i.namespaceURI() == "urn:xmpp:delay") {
            if (i.hasAttribute("stamp") && !stamp.isValid()) {
                stamp = QDateTime::fromString(i.attribute("stamp").left(19), Qt::ISODate);
            }
        } else if (i.tagName() == "x" && i.namespaceURI() == "gabber:x:music:info") {
            QDomElement t;
            QString     title, state;

            t = i.firstChildElement("title");
            if (!t.isNull())
                title = tagContent(t);
            t = i.firstChildElement("state");
            if (!t.isNull())
                state = tagContent(t);

            if (!title.isEmpty() && state == "playing")
                p.setSongTitle(title);
        } else if (i.tagName() == "x" && i.namespaceURI() == "jabber:x:signed") {
            p.setXSigned(tagContent(i));
        } else if (i.tagName() == "x" && i.namespaceURI() == "http://jabber.org/protocol/e2e") {
            p.setKeyID(tagContent(i));
        } else if (i.tagName() == "c" && i.namespaceURI() == NS_CAPS) {
            p.setCaps(CapsSpec::fromXml(i));
            if (!e.hasAttribute("type") && p.caps().isValid()) {
                client()->capsManager()->updateCaps(j, p.caps());
            }
        } else if (i.tagName() == "x" && i.namespaceURI() == "vcard-temp:x:update") {
            QDomElement t;
            t = i.firstChildElement("photo");
            if (!t.isNull())
                p.setPhotoHash(
                    QByteArray::fromHex(tagContent(t).toLatin1())); // if hash is empty this may mean photo removal
            // else vcard.hasPhotoHash() returns false and that's mean user is not yet ready to advertise his image
        } else if (i.tagName() == "x" && i.namespaceURI() == "http://jabber.org/protocol/muc#user") {
            for (QDomElement muc_e = i.firstChildElement(); !muc_e.isNull(); muc_e = muc_e.nextSiblingElement()) {
                if (muc_e.tagName() == "item")
                    p.setMUCItem(MUCItem(muc_e));
                else if (muc_e.tagName() == "status")
                    p.addMUCStatus(muc_e.attribute("code").toInt());
                else if (muc_e.tagName() == "destroy")
                    p.setMUCDestroy(MUCDestroy(muc_e));
            }
        } else if (i.tagName() == "data" && i.namespaceURI() == "urn:xmpp:bob") {
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

    emit presence(j, p);

    return true;
}

//----------------------------------------------------------------------------
// JT_Message
//----------------------------------------------------------------------------
JT_Message::JT_Message(Task *parent, Message &msg) : Task(parent), m(msg)
{
    if (msg.id().isEmpty())
        msg.setId(id());
}

JT_Message::~JT_Message() { }

void JT_Message::onGo()
{

    Stanza      s = m.toStanza(&(client()->stream()));
    QDomElement e = s.element();

    if (auto encryptionHandler = client()->encryptionHandler()) {
        Q_UNUSED(encryptionHandler->encryptMessageElement(e));
    }

    // See: XEP-0380: Explicit Message Encryption
    const bool wasEncrypted = !e.firstChildElement("encryption").isNull();
    m.setWasEncrypted(wasEncrypted);
    m.setEncryptionProtocol(encryptionProtocol(e));

    // if the element is null, then the encryption is happening asynchronously
    if (!e.isNull()) {
        send(e);
    }
    setSuccess();
}

//----------------------------------------------------------------------------
// JT_PushMessage
//----------------------------------------------------------------------------
class JT_PushMessage::Private {
public:
    EncryptionHandler *m_encryptionHandler;
};

JT_PushMessage::JT_PushMessage(Task *parent, EncryptionHandler *encryptionHandler) : Task(parent)
{
    d                      = new Private;
    d->m_encryptionHandler = encryptionHandler;
}

JT_PushMessage::~JT_PushMessage() { delete d; }

bool JT_PushMessage::take(const QDomElement &e)
{
    if (e.tagName() != "message")
        return false;

    QDomElement e1 = e;

    if (d->m_encryptionHandler) {
        if (d->m_encryptionHandler->decryptMessageElement(e1)) {
            if (e1.isNull()) {
                // The message was processed, but has to be discarded for some reason
                return true;
            }
        }
    }

    QDomElement        forward;
    Message::CarbonDir cd = Message::NoCarbon;

    Jid fromJid = Jid(e1.attribute(QLatin1String("from")));
    // Check for Carbon
    QDomNodeList list = e1.childNodes();
    for (int i = 0; i < list.size(); ++i) {
        QDomElement el = list.at(i).toElement();

        if (el.namespaceURI() == QLatin1String("urn:xmpp:carbons:2")
            && (el.tagName() == QLatin1String("received") || el.tagName() == QLatin1String("sent"))
            && fromJid.compare(Jid(e1.attribute(QLatin1String("to"))), false)) {
            QDomElement el1 = el.firstChildElement();
            if (el1.tagName() == QLatin1String("forwarded")
                && el1.namespaceURI() == QLatin1String("urn:xmpp:forward:0")) {
                QDomElement el2 = el1.firstChildElement(QLatin1String("message"));
                if (!el2.isNull()) {
                    forward = el2;
                    cd      = el.tagName() == QLatin1String("received") ? Message::Received : Message::Sent;
                    break;
                }
            }
        } else if (el.tagName() == QLatin1String("forwarded")
                   && el.namespaceURI() == QLatin1String("urn:xmpp:forward:0")) {
            forward = el.firstChildElement(QLatin1String("message")); // currently only messages are supportted
            // TODO <delay> element support
            if (!forward.isNull()) {
                break;
            }
        }
    }

    Stanza s = client()->stream().createStanza(addCorrectNS(forward.isNull() ? e1 : forward));
    if (s.isNull()) {
        // printf("take: bad stanza??\n");
        return false;
    }

    Message m;
    if (!m.fromStanza(s, client()->manualTimeZoneOffset(), client()->timeZoneOffset())) {
        // printf("bad message\n");
        return false;
    }
    if (!forward.isNull()) {
        m.setForwardedFrom(fromJid);
        m.setCarbonDirection(cd);
    }

    // See: XEP-0380: Explicit Message Encryption
    const bool wasEncrypted = !e1.firstChildElement("encryption").isNull();
    m.setWasEncrypted(wasEncrypted);
    m.setEncryptionProtocol(encryptionProtocol(e));

    emit message(m);
    return true;
}

//----------------------------------------------------------------------------
// JT_VCard
//----------------------------------------------------------------------------
class JT_VCard::Private {
public:
    Private() = default;

    QDomElement iq;
    Jid         jid;
    VCard       vcard;
};

JT_VCard::JT_VCard(Task *parent) : Task(parent)
{
    type = -1;
    d    = new Private;
}

JT_VCard::~JT_VCard() { delete d; }

void JT_VCard::get(const Jid &_jid)
{
    type          = 0;
    d->jid        = _jid;
    d->iq         = createIQ(doc(), "get", type == 1 ? Jid().full() : d->jid.full(), id());
    QDomElement v = doc()->createElementNS("vcard-temp", "vCard");
    d->iq.appendChild(v);
}

const Jid &JT_VCard::jid() const { return d->jid; }

const VCard &JT_VCard::vcard() const { return d->vcard; }

void JT_VCard::set(const VCard &card)
{
    type     = 1;
    d->vcard = card;
    d->jid   = "";
    d->iq    = createIQ(doc(), "set", d->jid.full(), id());
    d->iq.appendChild(card.toXml(doc()));
}

// isTarget is when we setting target's vCard. for example in case of muc own vCard
void JT_VCard::set(const Jid &j, const VCard &card, bool isTarget)
{
    type     = 1;
    d->vcard = card;
    d->jid   = j;
    d->iq    = createIQ(doc(), "set", isTarget ? j.full() : "", id());
    d->iq.appendChild(card.toXml(doc()));
}

void JT_VCard::onGo() { send(d->iq); }

bool JT_VCard::take(const QDomElement &x)
{
    Jid to = d->jid;
    if (to.bare() == client()->jid().bare())
        to = client()->host();
    if (!iqVerify(x, to, id()))
        return false;

    if (x.attribute("type") == "result") {
        if (type == 0) {
            for (QDomNode n = x.firstChild(); !n.isNull(); n = n.nextSibling()) {
                QDomElement q = n.toElement();
                if (q.isNull())
                    continue;

                if (q.tagName().toUpper() == "VCARD") {
                    d->vcard = VCard::fromXml(q);
                    if (d->vcard) {
                        setSuccess();
                        return true;
                    }
                }
            }

            setError(ErrDisc + 1, tr("No vCard available"));
            return true;
        } else {
            setSuccess();
            return true;
        }
    } else {
        setError(x);
    }

    return true;
}

//----------------------------------------------------------------------------
// JT_Search
//----------------------------------------------------------------------------
class JT_Search::Private {
public:
    Private() = default;

    Jid                 jid;
    Form                form;
    bool                hasXData = false;
    XData               xdata;
    QList<SearchResult> resultList;
};

JT_Search::JT_Search(Task *parent) : Task(parent)
{
    d    = new Private;
    type = -1;
}

JT_Search::~JT_Search() { delete d; }

void JT_Search::get(const Jid &jid)
{
    type              = 0;
    d->jid            = jid;
    d->hasXData       = false;
    d->xdata          = XData();
    iq                = createIQ(doc(), "get", d->jid.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:search", "query");
    iq.appendChild(query);
}

void JT_Search::set(const Form &form)
{
    type              = 1;
    d->jid            = form.jid();
    d->hasXData       = false;
    d->xdata          = XData();
    iq                = createIQ(doc(), "set", d->jid.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:search", "query");
    iq.appendChild(query);

    // key?
    if (!form.key().isEmpty())
        query.appendChild(textTag(doc(), "key", form.key()));

    // fields
    for (const auto &f : form) {
        query.appendChild(textTag(doc(), f.realName(), f.value()));
    }
}

void JT_Search::set(const Jid &jid, const XData &form)
{
    type              = 1;
    d->jid            = jid;
    d->hasXData       = false;
    d->xdata          = XData();
    iq                = createIQ(doc(), "set", d->jid.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:search", "query");
    iq.appendChild(query);
    query.appendChild(form.toXml(doc(), true));
}

const Form &JT_Search::form() const { return d->form; }

const QList<SearchResult> &JT_Search::results() const { return d->resultList; }

bool JT_Search::hasXData() const { return d->hasXData; }

const XData &JT_Search::xdata() const { return d->xdata; }

void JT_Search::onGo() { send(iq); }

bool JT_Search::take(const QDomElement &x)
{
    if (!iqVerify(x, d->jid, id()))
        return false;

    Jid from(x.attribute("from"));
    if (x.attribute("type") == "result") {
        if (type == 0) {
            d->form.clear();
            d->form.setJid(from);

            QDomElement q = queryTag(x);
            for (QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
                QDomElement i = n.toElement();
                if (i.isNull())
                    continue;

                if (i.tagName() == "instructions")
                    d->form.setInstructions(tagContent(i));
                else if (i.tagName() == "key")
                    d->form.setKey(tagContent(i));
                else if (i.tagName() == "x" && i.namespaceURI() == "jabber:x:data") {
                    d->xdata.fromXml(i);
                    d->hasXData = true;
                } else {
                    FormField f;
                    if (f.setType(i.tagName())) {
                        f.setValue(tagContent(i));
                        d->form += f;
                    }
                }
            }
        } else {
            d->resultList.clear();

            QDomElement q = queryTag(x);
            for (QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
                QDomElement i = n.toElement();
                if (i.isNull())
                    continue;

                if (i.tagName() == "item") {
                    SearchResult r(Jid(i.attribute("jid")));

                    QDomElement tag;

                    tag = i.firstChildElement("nick");
                    if (!tag.isNull())
                        r.setNick(tagContent(tag));
                    tag = i.firstChildElement("first");
                    if (!tag.isNull())
                        r.setFirst(tagContent(tag));
                    tag = i.firstChildElement("last");
                    if (!tag.isNull())
                        r.setLast(tagContent(tag));
                    tag = i.firstChildElement("email");
                    if (!tag.isNull())
                        r.setEmail(tagContent(tag));

                    d->resultList += r;
                } else if (i.tagName() == "x" && i.namespaceURI() == "jabber:x:data") {
                    d->xdata.fromXml(i);
                    d->hasXData = true;
                }
            }
        }
        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

//----------------------------------------------------------------------------
// JT_ClientVersion
//----------------------------------------------------------------------------
JT_ClientVersion::JT_ClientVersion(Task *parent) : Task(parent) { }

void JT_ClientVersion::get(const Jid &jid)
{
    j                 = jid;
    iq                = createIQ(doc(), "get", j.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:version", "query");
    iq.appendChild(query);
}

void JT_ClientVersion::onGo() { send(iq); }

bool JT_ClientVersion::take(const QDomElement &x)
{
    if (!iqVerify(x, j, id()))
        return false;

    if (x.attribute("type") == "result") {
        QDomElement q = queryTag(x);
        QDomElement tag;
        tag = q.firstChildElement("name");
        if (!tag.isNull())
            v_name = tagContent(tag);
        tag = q.firstChildElement("version");
        if (!tag.isNull())
            v_ver = tagContent(tag);
        tag = q.firstChildElement("os");
        if (!tag.isNull())
            v_os = tagContent(tag);

        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

const Jid &JT_ClientVersion::jid() const { return j; }

const QString &JT_ClientVersion::name() const { return v_name; }

const QString &JT_ClientVersion::version() const { return v_ver; }

const QString &JT_ClientVersion::os() const { return v_os; }

//----------------------------------------------------------------------------
// JT_EntityTime
//----------------------------------------------------------------------------
JT_EntityTime::JT_EntityTime(Task *parent) : Task(parent) { }

/**
 * \brief Queried entity's JID.
 */
const Jid &JT_EntityTime::jid() const { return j; }

/**
 * \brief Prepares the task to get information from JID.
 */
void JT_EntityTime::get(const Jid &jid)
{
    j                = jid;
    iq               = createIQ(doc(), "get", jid.full(), id());
    QDomElement time = doc()->createElementNS("urn:xmpp:time", "time");
    iq.appendChild(time);
}

void JT_EntityTime::onGo() { send(iq); }

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
        } while (false);
        setError(406);
    } else {
        setError(x);
    }

    return true;
}

const QDateTime &JT_EntityTime::dateTime() const { return utc; }

int JT_EntityTime::timezoneOffset() const { return tzo; }

//----------------------------------------------------------------------------
// JT_ServInfo
//----------------------------------------------------------------------------
JT_ServInfo::JT_ServInfo(Task *parent) : Task(parent) { }

JT_ServInfo::~JT_ServInfo() { }

bool JT_ServInfo::take(const QDomElement &e)
{
    if (e.tagName() != "iq" || e.attribute("type") != "get")
        return false;

    QString ns = queryNS(e);
    if (ns == "jabber:iq:version") {
        QDomElement iq    = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
        QDomElement query = doc()->createElementNS("jabber:iq:version", "query");
        iq.appendChild(query);
        query.appendChild(textTag(doc(), "name", client()->clientName()));
        query.appendChild(textTag(doc(), "version", client()->clientVersion()));
        query.appendChild(textTag(doc(), "os", client()->OSName() + ' ' + client()->OSVersion()));
        send(iq);
        return true;
    } else if (ns == "http://jabber.org/protocol/disco#info") {
        // Find out the node
        QString     node;
        QDomElement q = e.firstChildElement("query");
        if (!q.isNull()) // NOTE: Should always be true, since a NS was found above
            node = q.attribute("node");

        if (node.isEmpty() || node == client()->caps().flatten()) {

            QDomElement iq   = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
            DiscoItem   item = client()->makeDiscoResult(node);
            iq.appendChild(item.toDiscoInfoResult(doc()));
            send(iq);
        } else {
            // Create error reply
            QDomElement error_reply = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));

            // Copy children
            for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
                error_reply.appendChild(n.cloneNode());
            }

            // Add error
            QDomElement error = doc()->createElement("error");
            error.setAttribute("type", "cancel");
            error_reply.appendChild(error);
            QDomElement error_type = doc()->createElementNS("urn:ietf:params:xml:ns:xmpp-stanzas", "item-not-found");
            error.appendChild(error_type);
            send(error_reply);
        }
        return true;
    }
    if (!ns.isEmpty()) {
        return false;
    }

    ns = e.firstChildElement("time").namespaceURI();
    if (ns == "urn:xmpp:time") {
        QDomElement iq   = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
        QDomElement time = doc()->createElementNS(ns, "time");
        iq.appendChild(time);

        QDateTime local = QDateTime::currentDateTime();

        int     off = TimeZone::offsetFromUtc();
        QTime   t   = QTime(0, 0).addSecs(qAbs(off) * 60);
        QString tzo = (off < 0 ? "-" : "+") + t.toString("HH:mm");
        time.appendChild(textTag(doc(), "tzo", tzo));
        QString localTimeStr = local.toUTC().toString(Qt::ISODate);
        if (!localTimeStr.endsWith("Z"))
            localTimeStr.append("Z");
        time.appendChild(textTag(doc(), "utc", localTimeStr));

        send(iq);
        return true;
    }

    return false;
}

//----------------------------------------------------------------------------
// JT_Gateway
//----------------------------------------------------------------------------
JT_Gateway::JT_Gateway(Task *parent) : Task(parent) { type = -1; }

void JT_Gateway::get(const Jid &jid)
{
    type              = 0;
    v_jid             = jid;
    iq                = createIQ(doc(), "get", v_jid.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:gateway", "query");
    iq.appendChild(query);
}

void JT_Gateway::set(const Jid &jid, const QString &prompt)
{
    type              = 1;
    v_jid             = jid;
    v_prompt          = prompt;
    iq                = createIQ(doc(), "set", v_jid.full(), id());
    QDomElement query = doc()->createElementNS("jabber:iq:gateway", "query");
    iq.appendChild(query);
    query.appendChild(textTag(doc(), "prompt", v_prompt));
}

void JT_Gateway::onGo() { send(iq); }

Jid JT_Gateway::jid() const { return v_jid; }

QString JT_Gateway::desc() const { return v_desc; }

QString JT_Gateway::prompt() const { return v_prompt; }

Jid JT_Gateway::translatedJid() const { return v_translatedJid; }

bool JT_Gateway::take(const QDomElement &x)
{
    if (!iqVerify(x, v_jid, id()))
        return false;

    if (x.attribute("type") == "result") {
        if (type == 0) {
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
        } else {
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
    } else {
        setError(x);
    }

    return true;
}

//----------------------------------------------------------------------------
// JT_DiscoItems
//----------------------------------------------------------------------------
class JT_DiscoItems::Private {
public:
    Private() { }

    QDomElement iq;
    Jid         jid;
    DiscoList   items;
    QDomElement subsetsEl;
};

JT_DiscoItems::JT_DiscoItems(Task *parent) : Task(parent) { d = new Private; }

JT_DiscoItems::~JT_DiscoItems() { delete d; }

void JT_DiscoItems::get(const DiscoItem &item) { get(item.jid(), item.node()); }

void JT_DiscoItems::get(const Jid &j, const QString &node)
{
    d->items.clear();

    d->jid            = j;
    d->iq             = createIQ(doc(), "get", d->jid.full(), id());
    QDomElement query = doc()->createElementNS("http://jabber.org/protocol/disco#items", "query");

    if (!node.isEmpty())
        query.setAttribute("node", node);

    if (!d->subsetsEl.isNull()) {
        query.appendChild(d->subsetsEl);
        d->subsetsEl = QDomElement();
    }

    d->iq.appendChild(query);
}

const DiscoList &JT_DiscoItems::items() const { return d->items; }

void JT_DiscoItems::includeSubsetQuery(const SubsetsClientManager &subsets)
{
    d->subsetsEl = subsets.makeQueryElement(doc());
}

bool JT_DiscoItems::extractSubsetInfo(SubsetsClientManager &subsets)
{
    return d->subsetsEl.isNull() ? false : subsets.updateFromElement(d->subsetsEl, d->items.count());
}

void JT_DiscoItems::onGo() { send(d->iq); }

bool JT_DiscoItems::take(const QDomElement &x)
{
    if (!iqVerify(x, d->jid, id()))
        return false;

    if (x.attribute("type") == "result") {
        QDomElement q = queryTag(x);

        for (QDomNode n = q.firstChild(); !n.isNull(); n = n.nextSibling()) {
            QDomElement e = n.toElement();
            if (e.isNull())
                continue;

            if (e.tagName() == "item") {
                DiscoItem item;

                item.setJid(e.attribute("jid"));
                item.setName(e.attribute("name"));
                item.setNode(e.attribute("node"));
                item.setAction(DiscoItem::string2action(e.attribute("action")));

                d->items.append(item);
            } else if (d->subsetsEl.isNull()) {
                d->subsetsEl = SubsetsClientManager::findElement(e, false);
            }
        }

        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

//----------------------------------------------------------------------------
// JT_DiscoPublish
//----------------------------------------------------------------------------
class JT_DiscoPublish::Private {
public:
    Private() { }

    QDomElement iq;
    Jid         jid;
    DiscoList   list;
};

JT_DiscoPublish::JT_DiscoPublish(Task *parent) : Task(parent) { d = new Private; }

JT_DiscoPublish::~JT_DiscoPublish() { delete d; }

void JT_DiscoPublish::set(const Jid &j, const DiscoList &list)
{
    d->list = list;
    d->jid  = j;

    d->iq             = createIQ(doc(), "set", d->jid.full(), id());
    QDomElement query = doc()->createElementNS("http://jabber.org/protocol/disco#items", "query");

    // FIXME: unsure about this
    // if ( !node.isEmpty() )
    //    query.setAttribute("node", node);

    for (const auto &discoItem : list) {
        QDomElement w = doc()->createElement("item");

        w.setAttribute("jid", discoItem.jid().full());
        if (!discoItem.name().isEmpty())
            w.setAttribute("name", discoItem.name());
        if (!discoItem.node().isEmpty())
            w.setAttribute("node", discoItem.node());
        w.setAttribute("action", DiscoItem::action2string(discoItem.action()));

        query.appendChild(w);
    }

    d->iq.appendChild(query);
}

void JT_DiscoPublish::onGo() { send(d->iq); }

bool JT_DiscoPublish::take(const QDomElement &x)
{
    if (!iqVerify(x, d->jid, id()))
        return false;

    if (x.attribute("type") == "result") {
        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

// ---------------------------------------------------------
// JT_BoBServer
// ---------------------------------------------------------
JT_BoBServer::JT_BoBServer(Task *parent) : Task(parent) { }

bool JT_BoBServer::take(const QDomElement &e)
{
    if (e.tagName() != "iq" || e.attribute("type") != "get")
        return false;

    QDomElement data = e.firstChildElement("data");
    if (data.namespaceURI() == "urn:xmpp:bob") {
        QDomElement iq;
        BoBData     bd = client()->bobManager()->bobData(data.attribute("cid"));
        if (bd.isNull()) {
            iq = createIQ(client()->doc(), "error", e.attribute("from"), e.attribute("id"));
            Stanza::Error error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::ItemNotFound);
            iq.appendChild(error.toXml(*doc(), client()->stream().baseNS()));
        } else {
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
class JT_BitsOfBinary::Private {
public:
    Private() { }

    QDomElement iq;
    Jid         jid;
    QString     cid;
    BoBData     data;
};

JT_BitsOfBinary::JT_BitsOfBinary(Task *parent) : Task(parent) { d = new Private; }

JT_BitsOfBinary::~JT_BitsOfBinary() { delete d; }

void JT_BitsOfBinary::get(const Jid &j, const QString &cid)
{
    d->jid = j;
    d->cid = cid;

    d->data = client()->bobManager()->bobData(cid);
    if (d->data.isNull()) {
        d->iq            = createIQ(doc(), "get", d->jid.full(), id());
        QDomElement data = doc()->createElementNS("urn:xmpp:bob", "data");
        data.setAttribute("cid", cid);
        d->iq.appendChild(data);
    }
}

void JT_BitsOfBinary::onGo()
{
    if (d->data.isNull()) {
        send(d->iq);
    } else {
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
    } else {
        setError(x);
    }

    return true;
}

BoBData &JT_BitsOfBinary::data() { return d->data; }

//----------------------------------------------------------------------------
// JT_PongServer
//----------------------------------------------------------------------------
/**
 * \class JT_PongServer
 * \brief Answers XMPP Pings
 */

JT_PongServer::JT_PongServer(Task *parent) : Task(parent) { }

bool JT_PongServer::take(const QDomElement &e)
{
    if (e.tagName() != "iq" || e.attribute("type") != "get")
        return false;

    QDomElement ping = e.firstChildElement("ping");
    if (!e.isNull() && ping.namespaceURI() == "urn:xmpp:ping") {
        QDomElement iq = createIQ(doc(), "result", e.attribute("from"), e.attribute("id"));
        send(iq);
        return true;
    }
    return false;
}

//---------------------------------------------------------------------------
// JT_CaptchaChallenger
//---------------------------------------------------------------------------
class JT_CaptchaChallenger::Private {
public:
    Jid              j;
    CaptchaChallenge challenge;
};

JT_CaptchaChallenger::JT_CaptchaChallenger(Task *parent) : Task(parent), d(new Private) { }

JT_CaptchaChallenger::~JT_CaptchaChallenger() { delete d; }

void JT_CaptchaChallenger::set(const Jid &j, const CaptchaChallenge &c)
{
    d->j         = j;
    d->challenge = c;
}

void JT_CaptchaChallenger::onGo()
{
    setTimeout(CaptchaValidTimeout);

    Message m;
    m.setId(id());
    m.setBody(d->challenge.explanation());
    m.setUrlList(d->challenge.urls());

    XData                      form = d->challenge.form();
    XData::FieldList           fl   = form.fields();
    XData::FieldList::Iterator it;
    for (it = fl.begin(); it < fl.end(); ++it) {
        if (it->var() == "challenge" && it->type() == XData::Field::Field_Hidden) {
            it->setValue(QStringList() << id());
        }
    }
    if (it == fl.end()) {
        XData::Field f;
        f.setType(XData::Field::Field_Hidden);
        f.setVar("challenge");
        f.setValue(QStringList() << id());
        fl.append(f);
    }
    form.setFields(fl);

    m.setForm(form);
    m.setTo(d->j);
    client()->sendMessage(m);
}

bool JT_CaptchaChallenger::take(const QDomElement &x)
{
    if (x.tagName() == "message" && x.attribute("id") == id() && Jid(x.attribute("from")) == d->j
        && !x.firstChildElement("error").isNull()) {
        setError(x);
        return true;
    }

    XDomNodeList nl;
    XData        xd;
    QString      rid = x.attribute("id");
    if (rid.isEmpty() || x.tagName() != "iq" || Jid(x.attribute("from")) != d->j || x.attribute("type") != "set"
        || (nl = childElementsByTagNameNS(x, "urn:xmpp:captcha", "captcha")).isEmpty()
        || (nl = childElementsByTagNameNS(nl.item(0).toElement(), "jabber:x:data", "x")).isEmpty()
        || (xd.fromXml(nl.item(0).toElement()), xd.getField("challenge").value().value(0) != id())) {
        return false;
    }

    CaptchaChallenge::Result r = d->challenge.validateResponse(xd);
    QDomElement              iq;
    if (r == CaptchaChallenge::Passed) {
        iq = createIQ(doc(), "result", d->j.full(), rid);
    } else {
        Stanza::Error::ErrorCond ec;
        if (r == CaptchaChallenge::Unavailable) {
            ec = Stanza::Error::ErrorCond::ServiceUnavailable;
        } else {
            ec = Stanza::Error::ErrorCond::NotAcceptable;
        }
        iq = createIQ(doc(), "error", d->j.full(), rid);
        Stanza::Error error(Stanza::Error::ErrorType::Cancel, ec);
        iq.appendChild(error.toXml(*doc(), client()->stream().baseNS()));
    }
    send(iq);

    setSuccess();

    return true;
}

//---------------------------------------------------------------------------
// JT_CaptchaSender
//---------------------------------------------------------------------------
JT_CaptchaSender::JT_CaptchaSender(Task *parent) : Task(parent) { }

void JT_CaptchaSender::set(const Jid &j, const XData &xd)
{
    to = j;

    iq = createIQ(doc(), "set", to.full(), id());
    iq.appendChild(doc()->createElementNS("urn:xmpp:captcha", "captcha")).appendChild(xd.toXml(doc(), true));
}

void JT_CaptchaSender::onGo() { send(iq); }

bool JT_CaptchaSender::take(const QDomElement &x)
{
    if (!iqVerify(x, to, id())) {
        return false;
    }

    if (x.attribute("type") == "result") {
        setSuccess();
    } else {
        setError(x);
    }

    return true;
}

//----------------------------------------------------------------------------
// JT_MessageCarbons
//----------------------------------------------------------------------------
JT_MessageCarbons::JT_MessageCarbons(Task *parent) : Task(parent) { }

void JT_MessageCarbons::enable()
{
    _iq = createIQ(doc(), "set", "", id());

    QDomElement enable = doc()->createElementNS("urn:xmpp:carbons:2", "enable");

    _iq.appendChild(enable);
}

void JT_MessageCarbons::disable()
{
    _iq = createIQ(doc(), "set", "", id());

    QDomElement disable = doc()->createElementNS("urn:xmpp:carbons:2", "disable");

    _iq.appendChild(disable);
}

void JT_MessageCarbons::onGo()
{
    send(_iq);
    setSuccess();
}

bool JT_MessageCarbons::take(const QDomElement &e)
{
    if (e.tagName() != "iq" || e.attribute("type") != "result")
        return false;

    bool res = iqVerify(e, Jid(), id());
    return res;
}
