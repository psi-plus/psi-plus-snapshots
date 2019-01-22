/*
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

#include "jingle.h"
#include "xmpp_xmlcommon.h"
#include "xmpp/jid/jid.h"
#include "xmpp-im/xmpp_hash.h"

#include <QDateTime>
#include <QDomElement>
#include <QMap>
#include <QStringLiteral>

namespace XMPP {
namespace Jingle {

//----------------------------------------------------------------------------
// Jingle
//----------------------------------------------------------------------------
static const struct {
    const char *text;
    Jingle::Action action;
} jingleActions[] = {
{ "content-accept",     Jingle::ContentAccept },
{ "content-add",        Jingle::ContentAdd },
{ "content-modify",     Jingle::ContentModify },
{ "content-reject",     Jingle::ContentReject },
{ "content-remove",     Jingle::ContentRemove },
{ "description-info",   Jingle::DescriptionInfo },
{ "security-info",      Jingle::SecurityInfo },
{ "session-accept",     Jingle::SessionAccept },
{ "session-info",       Jingle::SessionInfo },
{ "session-initiate",   Jingle::SessionInitiate },
{ "session-terminate",  Jingle::SessionTerminate },
{ "transport-accept",   Jingle::TransportAccept },
{ "transport-info",     Jingle::TransportInfo },
{ "transport-reject",   Jingle::TransportReject },
{ "transport-replace",  Jingle::TransportReplace }
};

class Jingle::Private : public QSharedData
{
public:
    Jingle::Action action;
    QString sid;
    Jid initiator;
    Jid responder;
    QList<Content> content;
    Reason reason;
};

Jingle::Jingle(const QDomElement &e)
{
    QString actionStr = e.attribute(QLatin1String("action"));
    Action action;
    Reason reason;
    QString sid = e.attribute(QLatin1String("sid"));
    QList<Content> contentList;
    Jid initiator;
    Jid responder;


    bool found = false;
    for (unsigned int i = 0; i < sizeof(jingleActions) / sizeof(jingleActions[0]); i++) {
        if (actionStr == jingleActions[i].text) {
            found = true;
            action = jingleActions[i].action;
            break;
        }
    }
    if (!found || sid.isEmpty()) {
        return;
    }

    QDomElement re = e.firstChildElement(QLatin1String("reason"));
    if(!re.isNull()) {
        reason = Reason(re);
        if (!reason.isValid()) {
            qDebug("invalid reason");
            return;
        }
    }
    for(QDomElement ce = e.firstChildElement(QLatin1String("content"));
        !ce.isNull(); ce = ce.nextSiblingElement(QLatin1String("content"))) {

        auto c = Content(ce);
        if (!c.isValid()) {
            qDebug("invalid content");
            return;
        }

        contentList += c;
    }
    if (!e.attribute(QLatin1String("initiator")).isEmpty()) {
        initiator = Jid(e.attribute(QLatin1String("initiator")));
        if (initiator.isNull()) {
            qDebug("malformed initiator jid");
            return;
        }
    }
    if (!e.attribute(QLatin1String("responder")).isEmpty()) {
        responder = Jid(e.attribute(QLatin1String("responder")));
        if (responder.isNull()) {
            qDebug("malformed responder jid");
            return;
        }
    }

    d = new Private;
    d->action = action;
    d->sid = sid;
    d->reason = reason;
    d->content = contentList;
    d->initiator = initiator;
    d->responder = responder;
}

Jingle::Private* Jingle::ensureD()
{
    if (!d) {
        d = new Private;
    }
    return d.data();
}

QDomElement Jingle::toXml(QDomDocument *doc) const
{
    if (!d || d->sid.isEmpty() || d->action == NoAction || (!d->reason.isValid() && d->content.isEmpty())) {
        return QDomElement();
    }

    QDomElement query = doc->createElementNS(QLatin1String(JINGLE_NS), QLatin1String("jingle"));
    //query.setAttribute("xmlns", JINGLE_NS);
    for (unsigned int i = 0; i < sizeof(jingleActions) / sizeof(jingleActions[0]); i++) {
        if (jingleActions[i].action == d->action) {
            query.setAttribute(QLatin1String("action"), QLatin1String(jingleActions[i].text));
            break;
        }
    }

    if(!d->initiator.isNull())
        query.setAttribute(QLatin1String("initiator"), d->initiator.full());
    if(!d->responder.isNull())
        query.setAttribute(QLatin1String("responder"), d->responder.full());
    query.setAttribute(QLatin1String("sid"), d->sid);

    if(d->action != SessionTerminate) {
        // for session terminate, there is no content list, just
        //   a reason for termination
        for(const Content &c: d->content) {
            QDomElement content = c.toXml(doc);
            query.appendChild(content);
        }
    }
    if (d->reason.isValid()) {
        query.appendChild(d->reason.toXml(doc));
    }
    return query;
}



//----------------------------------------------------------------------------
// Reason
//----------------------------------------------------------------------------
static const QMap<QString,Reason::Condition> reasonConditions = {
    { QStringLiteral("alternative-session"),      Reason::AlternativeSession },
    { QStringLiteral("busy"),                     Reason::Busy },
    { QStringLiteral("cancel"),                   Reason::Cancel },
    { QStringLiteral("connectivity-error"),       Reason::ConnectivityError },
    { QStringLiteral("decline"),                  Reason::Decline },
    { QStringLiteral("expired"),                  Reason::Expired },
    { QStringLiteral("failed-application"),       Reason::FailedApplication },
    { QStringLiteral("failed-transport"),         Reason::FailedTransport },
    { QStringLiteral("general-error"),            Reason::GeneralError },
    { QStringLiteral("gone"),                     Reason::Gone },
    { QStringLiteral("incompatible-parameters"),  Reason::IncompatibleParameters },
    { QStringLiteral("media-error"),              Reason::MediaError },
    { QStringLiteral("security-error"),           Reason::SecurityError },
    { QStringLiteral("success"),                  Reason::Success },
    { QStringLiteral("timeout"),                  Reason::Timeout },
    { QStringLiteral("unsupported-applications"), Reason::UnsupportedApplications },
    { QStringLiteral("unsupported-transports"),   Reason::UnsupportedTransports },
};

class Reason::Private :public QSharedData {
public:
    Reason::Condition cond;
    QString text;
};

Reason::Reason(const QDomElement &e)
{
    if(e.tagName() != QLatin1String("reason"))
        return;

    Condition condition = NoReason;
    QString text;

    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        if (c.tagName() == QLatin1String("text")) {
            text = c.text();
        }
        else if (c.namespaceURI() != e.namespaceURI()) {
            // TODO add here all the extensions to reason.
        }
        else {
            condition = reasonConditions.value(c.tagName());
        }
    }

    if (condition != NoReason) {
        d = new Private;
        d->cond = condition;
        d->text = text;
    }
}

Reason::Condition Reason::condition() const
{
    if (d) return d->cond;
    return NoReason;
}

void Reason::setCondition(Condition cond)
{
    ensureD()->cond = cond;
}

QString Reason::text() const
{
    if (d) return d->text;
    return QString();
}

void Reason::setText(const QString &text)
{
    ensureD()->text = text;
}

QDomElement Reason::toXml(QDomDocument *doc) const
{
    if (d && d->cond != NoReason) {
        for (auto r = reasonConditions.cbegin(); r != reasonConditions.cend(); ++r) {
            if (r.value() == d->cond) {
                QDomElement e = doc->createElement(QLatin1String("reason"));
                e.appendChild(doc->createElement(r.key()));
                if (!d->text.isEmpty()) {
                    e.appendChild(textTag(doc, QLatin1String("text"), d->text));
                }
                return e;
            }
        }
    }
    return QDomElement();
}

Reason::Private* Reason::ensureD()
{
    if (!d) {
        d = new Private;
    }
    return d.data();
}

//----------------------------------------------------------------------------
// Content
//----------------------------------------------------------------------------
ContentBase::ContentBase(const QDomElement &el)
{
    creator = creatorAttr(el);
    name = el.attribute(QLatin1String("name"));
}

QDomElement ContentBase::toXml(QDomDocument *doc, const char *tagName) const
{
    if (!isValid()) {
        return QDomElement();
    }
    auto el = doc->createElement(QLatin1String(tagName));
    setCreatorAttr(el, creator);
    el.setAttribute(QLatin1String("name"), name);
    return el;
}


Content::Creator ContentBase::creatorAttr(const QDomElement &el)
{
    auto creatorStr = el.attribute(QLatin1String("creator"));
    if (creatorStr == QLatin1String("initiator")) {
        return Content::Creator::Initiator;
    }
    if (creatorStr == QLatin1String("responder")) {
        return Content::Creator::Responder;
    }
    return Content::Creator::NoCreator;
}

bool ContentBase::setCreatorAttr(QDomElement &el, Content::Creator creator)
{
    if (creator == Content::Creator::Initiator) {
        el.setAttribute(QLatin1String("creator"), QLatin1String("initiator"));
    } else if (creator == Content::Creator::Responder) {
        el.setAttribute(QLatin1String("creator"), QLatin1String("responder"));
    } else {
        return false;
    }
    return true;
}

class Content::Private : public QSharedData
{
public:
    Content::Senders senders;
    QString disposition; // default "session"
};

Content::Content(const QDomElement &content) :
    ContentBase(content)
{
    if (!ContentBase::isValid()) {
        return;
    }

    QString disposition = QLatin1String("session");
	Senders senders = Senders::Both;
    //Xmlable *description = 0, *transport = 0, *security = 0;

	QDomElement descriptionEl = content.firstChildElement(QLatin1String("description"));
	QDomElement transportEl = content.firstChildElement(QLatin1String("transport"));
	//QDomElement securityEl = content.firstChildElement(QLatin1String("security"));
	if (descriptionEl.isNull() || transportEl.isNull()) {
		return;
	}
	QString sendersStr = content.attribute(QLatin1String("senders"));
	if (sendersStr == QLatin1String("initiator")) {
		senders = Senders::Initiator;
	}
	else if (sendersStr == QLatin1String("none")) {
		senders = Senders::None;
	}
	else if (sendersStr == QLatin1String("responder")) {
		senders = Senders::Responder;
	}
	else if (sendersStr == QLatin1String("both")) {
		senders = Senders::Both;
	}
	else {
		return;
	}
	disposition = content.attribute(QLatin1String("disposition"));
	/*
    description = client->jingleManager()->descriptionFromXml(descriptionEl);
	transport = client->jingleManager()->transportFromXml(transportEl);
	if (!securityEl.isNull()) {
		security = client->jingleManager()->securityFromXml(securityEl);
		// if security == 0 then then its unsupported? just ignore it atm
		// according to xtls draft responder may omit security when unsupported.
	}
    */
    d = new Private;
	d->senders = senders;
	d->disposition = disposition;
    // TODO description
    // TODO transports
    // TODO security
}

QDomElement Content::toXml(QDomDocument *doc) const
{
    QString creatorStr;
	QString sendersStr;

    auto el = ContentBase::toXml(doc, "content");
    if (el.isNull()) {  // TODO check/init other elements of content
        return el;
    }

	switch (d->senders) {
		case Senders::None:
			sendersStr = QLatin1String("none");
			break;

		case Senders::Initiator:
			sendersStr = QLatin1String("initiator");
			break;

		case Senders::Responder:
			sendersStr = QLatin1String("responder");
			break;

		case Senders::Both:
		default:
			break;
	}

	QDomElement content = doc->createElement(QLatin1String("content"));
	content.setAttribute(QLatin1String("creator"), creatorStr);
	if (d->disposition != QLatin1String("session")) {
		content.setAttribute(QLatin1String("disposition"), d->disposition); // NOTE review how we can parse it some generic way
	}
	if (!sendersStr.isEmpty()) {
		content.setAttribute(QLatin1String("senders"), sendersStr);
	}

    /*
	content.appendChild(description->toXml(doc));
	content.appendChild(transport->toXml(doc));
	if (!security.isNull()) {
		content.appendChild(security->toXml(doc));
	}
    */
	return content;
}

Content::Private *Content::ensureD()
{
    if (!d) {
        d = new Private;
    }
    return d.data();
}


namespace FileTransfer {

static const QString NS = QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5");

//----------------------------------------------------------------------------
// File
//----------------------------------------------------------------------------
class File::Private : public QSharedData
{
public:
    QDateTime date;
    QString mediaType;
    QString name;
    QString desc;
    QString size;
    Range range;
    Hash hash;
};

File::File(const QDomElement &file)
{
    QDateTime date;
    QString mediaType;
    QString name;
    QString desc;
    size_t size;
    Range range{};
    Hash hash;

    bool ok;

    for(QDomElement ce = file.firstChildElement();
        !ce.isNull(); ce = ce.nextSiblingElement()) {

        if (ce.tagName() == QLatin1String("date")) {
            date = QDateTime::fromString(ce.text().left(19), Qt::ISODate);
            if (!date.isValid()) {
                return;
            }

        } else if (ce.tagName() == QLatin1String("media-type")) {
            mediaType = ce.text();

        } else if (ce.tagName() == QLatin1String("name")) {
            name = ce.text();

        } else if (ce.tagName() == QLatin1String("size")) {
            size = ce.text().toULongLong(&ok);
            if (!ok) {
                return;
            }

        } else if (ce.tagName() == QLatin1String("range")) {
            if (ce.hasAttribute(QLatin1String("offset"))) {
                range.offset = ce.attribute(QLatin1String("offset")).toULongLong(&ok);
                if (!ok) {
                    return;
                }
            }
            if (ce.hasAttribute(QLatin1String("length"))) {
                range.offset = ce.attribute(QLatin1String("length")).toULongLong(&ok);
                if (!ok) {
                    return;
                }
            }
            QDomElement hashEl = ce.firstChildElement(QLatin1String("hash"));
            if (hashEl.namespaceURI() == QLatin1String("urn:xmpp:hashes:2")) {
                range.hash = Hash(hashEl);
                if (range.hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

        } else if (ce.tagName() == QLatin1String("desc")) {
            desc = ce.text();

        } else if (ce.tagName() == QLatin1String("hash")) {
            if (ce.namespaceURI() == QLatin1String("urn:xmpp:hashes:2")) {
                hash = Hash(ce);
                if (hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

        } else if (ce.tagName() == QLatin1String("hash-used")) {
            if (ce.namespaceURI() == QLatin1String("urn:xmpp:hashes:2")) {
                hash = Hash(ce);
                if (hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

        }
    }

    // TODO make private and fill it
}

QDomElement File::toXml(QDomDocument *doc) const
{
    return QDomElement(); // TODO
}

File::Private *File::ensureD()
{
    if (!d) {
        d = new Private;
    }
    return d.data();
}

//----------------------------------------------------------------------------
// Checksum
//----------------------------------------------------------------------------
Checksum::Checksum(const QDomElement &cs) :
    ContentBase(cs)
{
    file = File(cs.firstChildElement(QLatin1String("file")));
}

bool Checksum::isValid() const
{
    return ContentBase::isValid() && file.isValid();
}

QDomElement Checksum::toXml(QDomDocument *doc) const
{
    auto el = ContentBase::toXml(doc, "checksum");
    if (!el.isNull()) {
        el.appendChild(file.toXml(doc));
    }
    return el;
}

//----------------------------------------------------------------------------
// Received
//----------------------------------------------------------------------------
QDomElement Received::toXml(QDomDocument *doc) const
{
    return ContentBase::toXml(doc, "received");
}


} // namespace FileTransfer

} // namespace Jingle
} // namespace XMPP
