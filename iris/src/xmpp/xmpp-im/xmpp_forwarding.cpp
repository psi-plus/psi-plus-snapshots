/*
 * xmpp_forwarding.cpp - Stanza Forwarding (XEP-0297)
 * Copyright (C) 2019  Aleksey Andreev
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

#include "xmpp_forwarding.h"
#include "xmpp_client.h"
#include "xmpp_message.h"
#include "xmpp_stream.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {

static const QString xmlns_forward(QStringLiteral("urn:xmpp:forward:0"));
static const QString xmlns_delay(QStringLiteral("urn:xmpp:delay"));

//--------------------------------------------------
// class Forwarding
//--------------------------------------------------

Forwarding::Forwarding() : type_(ForwardedNone) { }

Forwarding::Forwarding(const Forwarding &other) : type_(other.type_), ts_(other.ts_), msg_(other.msg_) { }

Forwarding::~Forwarding() { }

Forwarding &Forwarding::operator=(const Forwarding &from)
{
    if (this != &from) {
        type_ = from.type_;
        ts_   = from.ts_;
        msg_  = from.msg_;
    }
    return *this;
}

Forwarding::Type Forwarding::type() const { return type_; }

void Forwarding::setType(Type type)
{
    if (type_ != type) {
        type_ = type;
        if (type == ForwardedNone) {
            ts_  = QDateTime();
            msg_ = Message();
        }
    }
}

bool Forwarding::isCarbons() const { return (type_ == ForwardedCarbonsSent || type_ == ForwardedCarbonsReceived); }

QDateTime Forwarding::timeStamp() const
{
    if (!ts_.isNull())
        return ts_;
    return msg_.timeStamp();
}

void Forwarding::setTimeStamp(const QDateTime &ts) { ts_ = ts; }

Message Forwarding::message() const { return msg_; }

void Forwarding::setMessage(const Message &msg) { msg_ = msg; }

bool Forwarding::fromXml(const QDomElement &e, Client *client)
{
    if (e.tagName() != QString::fromLatin1("forwarded") || e.attribute(QString::fromLatin1("xmlns")) != xmlns_forward)
        return false;

    bool correct      = false;
    type_             = Forwarding::ForwardedNone;
    QDomElement child = e.firstChildElement();
    while (!child.isNull()) {
        if (child.tagName() == QString::fromLatin1("message")) {
            if (client->pushMessage()->processXmlSubscribers(child, client, true))
                break;
            Stanza  s = client->stream().createStanza(addCorrectNS(child));
            Message msg;
            if (msg.fromStanza(s, client->manualTimeZoneOffset(), client->timeZoneOffset())) {
                if (client->pushMessage()->processMessageSubscribers(msg, true))
                    break;
                msg_    = msg;
                type_   = ForwardedMessage;
                correct = true;
            }
        } else if (child.tagName() == QString::fromLatin1("delay")
                   && child.attribute(QString::fromLatin1("xmlns")) == xmlns_delay) {
            ts_ = QDateTime::fromString(child.attribute(QString::fromLatin1("stamp")).left(19), Qt::ISODate);
        }
        child = child.nextSiblingElement();
    }
    return correct;
}

QDomElement Forwarding::toXml(Stream *stream) const
{
    if (type_ == ForwardedNone || msg_.isNull())
        return QDomElement();

    QDomElement e = stream->doc().createElement(QString::fromLatin1("forwarded"));
    e.setAttribute(QString::fromLatin1("xmlns"), xmlns_forward);
    if (ts_.isValid()) {
        QDomElement delay = stream->doc().createElement(QString::fromLatin1("delay"));
        delay.setAttribute(QString::fromLatin1("xmlns"), xmlns_delay);
        delay.setAttribute(QString::fromLatin1("stamp"), ts_.toUTC().toString(Qt::ISODate) + "Z");
        e.appendChild(delay);
    }
    e.appendChild(msg_.toStanza(stream).element());
    return e;
}

//--------------------------------------------------
// class ForwardingManager
//--------------------------------------------------

class ForwardingSubscriber : public JT_PushMessage::Subscriber {
public:
    bool xmlEvent(const QDomElement &root, QDomElement &e, Client *c, int userData, bool nested) override
    {
        Q_UNUSED(root)
        Q_UNUSED(userData)
        frw.setType(Forwarding::ForwardedNone);
        if (!nested) {
            Stanza stanza = c->stream().createStanza(e);
            if (!stanza.isNull() && stanza.kind() == Stanza::Message) {
                frw.fromXml(e, c);
            }
        }
        return false;
    }

    bool messageEvent(Message &msg, int userData, bool nested) override
    {
        Q_UNUSED(userData)
        if (!nested && frw.type() != Forwarding::ForwardedNone) {
            msg.setForwarded(frw);
            frw.setType(Forwarding::ForwardedNone);
        }
        return false;
    }

private:
    Forwarding frw;
};

//--------------------------------------------------
// class ForwardingManager
//--------------------------------------------------

class ForwardingManager::Private {
public:
    ~Private()
    {
        //        if (sbs.get()) {
        //            push_m->unsubscribeXml(sbs.get(), QLatin1String("forwarded"), xmlns_forward);
        //            push_m->unsubscribeMessage(sbs.get());
        //        }
    }

    JT_PushMessage                       *push_m;
    std::unique_ptr<ForwardingSubscriber> sbs;
    bool                                  enabled = false;
};

ForwardingManager::ForwardingManager(JT_PushMessage *push_m) : QObject(push_m), d(new Private) { d->push_m = push_m; }

ForwardingManager::~ForwardingManager() { }

void ForwardingManager::setEnabled(bool enabled)
{
    if (d->enabled == enabled)
        return;

    if (enabled) {
        d->sbs.reset(new ForwardingSubscriber());
        d->push_m->subscribeXml(d->sbs.get(), QString::fromLatin1("forwarded"), xmlns_forward, 0);
        d->push_m->subscribeMessage(d->sbs.get(), 0);
    } else {
        d->push_m->unsubscribeXml(d->sbs.get(), QString::fromLatin1("forwarded"), xmlns_forward);
        d->push_m->unsubscribeMessage(d->sbs.get());
        d->sbs.reset();
    }
    d->enabled = enabled;
}

bool ForwardingManager::isEnabled() const { return d->enabled; }

} // namespace XMPP
