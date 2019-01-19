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

QDomElement Jingle::element(QDomDocument *doc) const
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
            QDomElement content = c.element(doc);
            query.appendChild(content);
        }
    }
    if (d->reason.isValid()) {
        query.appendChild(d->reason.element(doc));
    }
    return query;
}



//----------------------------------------------------------------------------
// Reason
//----------------------------------------------------------------------------
static const struct {
    const char *text;
    Reason::Condition cond;
} ReasonConditions[] =
{
{ "alternative-session",      Reason::AlternativeSession },
{ "busy",                     Reason::Busy },
{ "cancel",                   Reason::Cancel },
{ "connectivity-error",       Reason::ConnectivityError },
{ "decline",                  Reason::Decline },
{ "expired",                  Reason::Expired },
{ "failed-application",       Reason::FailedApplication },
{ "failed-transport",         Reason::FailedTransport },
{ "general-error",            Reason::GeneralError },
{ "gone",                     Reason::Gone },
{ "incompatible-parameters",  Reason::IncompatibleParameters },
{ "media-error",              Reason::MediaError },
{ "security-error",           Reason::SecurityError },
{ "success",                  Reason::Success },
{ "timeout",                  Reason::Timeout },
{ "unsupported-applications", Reason::UnsupportedApplications },
{ "unsupported-transports",   Reason::UnsupportedTransports },
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

    bool valid = false;
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
            for(int n = 0; sizeof(ReasonConditions) / sizeof(ReasonConditions[0]); ++n) {
                if(c.tagName() == QLatin1String(ReasonConditions[n].text)) {
                    condition = ReasonConditions[n].cond;
                    valid = true;
                    break;
                }
            }
        }
    }

    if (valid) {
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

QDomElement Reason::element(QDomDocument *doc) const
{
    if (d && d->cond != NoReason) {
        for(int n = 0; sizeof(ReasonConditions) / sizeof(ReasonConditions[0]); ++n) {
            if(ReasonConditions[n].cond == d->cond) {
                QDomElement e = doc->createElement(QLatin1String("reason"));
                e.appendChild(doc->createElement(QLatin1String(ReasonConditions[n].text)));
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
// Reason
//----------------------------------------------------------------------------
class Content::Private : public QSharedData
{
public:
    Content::Creator creator;
    Content::Senders senders;
    QString disposition; // default "session"
    QString name;
};

Content::Content(const QDomElement &content)
{
    QString name;
	QString disposition = QLatin1String("session");
	Creator creator;
	Senders senders = Senders::Both;
    //Xmlable *description = 0, *transport = 0, *security = 0;

	name = content.attribute(QLatin1String("name"));
	QDomElement descriptionEl = content.firstChildElement(QLatin1String("description"));
	QDomElement transportEl = content.firstChildElement(QLatin1String("transport"));
	//QDomElement securityEl = content.firstChildElement(QLatin1String("security"));
	if (name.isEmpty() || descriptionEl.isNull() || transportEl.isNull()) {
		return;
	}
	QString creatorStr = content.attribute(QLatin1String("name"));
	if (creatorStr == QLatin1String("initiator")) {
		creator = Creator::Initiator;
	}
	else if (creatorStr == QLatin1String("responder")) {
		creator = Creator::Responder;
	}
	else {
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
    d->creator = creator;
    d->name = name;
	d->senders = senders;
	d->disposition = disposition;
    // TODO description
    // TODO transports
    // TODO security
}

QDomElement Content::element(QDomDocument *doc) const
{
    QString creatorStr;
	QString sendersStr;

	switch (d->creator) {
    case Creator::Initiator:
		creatorStr = QLatin1String("initiator");
		break;
	case Creator::Responder:
		creatorStr = QLatin1String("responder");
		break;
	}
	if (creatorStr.isEmpty() || d->name.isEmpty()) { //TODO  || d->description.isNull() || d->transport.isNull()
		qDebug("invalid jingle content");
		return QDomElement();
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
	content.setAttribute(QLatin1String("name"), d->name);
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

//----------------------------------------------------------------------------
// Reason
//----------------------------------------------------------------------------
class FileTransfer::Private : public QSharedData
{
public:
    QDateTime date;
    QString mediaType;
    QString name;
    QString desc;
    QString size;
    FileTransfer::Range range;
    Hash hash;
};

FileTransfer::FileTransfer(const QDomElement &file)
{
    QDateTime date;
    QString mediaType;
    QString name;
    QString desc;
    QString size;
    Range range;
}


} // namespace Jingle
} // namespace XMPP
