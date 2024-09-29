/*
 * xmpp_carbons.cpp - Message Carbons (XEP-0280)
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

#include <QDomElement>

#include "xmpp_carbons.h"
#include "xmpp_client.h"
#include "xmpp_forwarding.h"
#include "xmpp_message.h"
#include "xmpp_task.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {

static const QString xmlns_carbons(QStringLiteral("urn:xmpp:carbons:2"));

class CarbonsSubscriber : public JT_PushMessage::Subscriber {
public:
    bool xmlEvent(const QDomElement &root, QDomElement &e, Client *client, int userData, bool nested) override;
    bool messageEvent(Message &msg, int userData, bool nested) override;

private:
    Forwarding frw;
};

class JT_MessageCarbons : public Task {
    Q_OBJECT

public:
    JT_MessageCarbons(Task *parent);

    void enable();
    void disable();

    void onGo() override;
    bool take(const QDomElement &e) override;

private:
    QDomElement iq;
};

//----------------------------------------------------------------------------
// JT_MessageCarbons
//----------------------------------------------------------------------------
JT_MessageCarbons::JT_MessageCarbons(Task *parent) : Task(parent) { }

void JT_MessageCarbons::enable()
{
    iq                 = createIQ(doc(), QString::fromLatin1("set"), QString(), id());
    QDomElement enable = doc()->createElement(QString::fromLatin1("enable"));
    enable.setAttribute(QString::fromLatin1("xmlns"), xmlns_carbons);
    iq.appendChild(enable);
}

void JT_MessageCarbons::disable()
{
    iq                  = createIQ(doc(), QString::fromLatin1("set"), QString(), id());
    QDomElement disable = doc()->createElement(QString::fromLatin1("disable"));
    disable.setAttribute(QString::fromLatin1("xmlns"), xmlns_carbons);
    iq.appendChild(disable);
}

void JT_MessageCarbons::onGo()
{
    if (!iq.isNull())
        send(iq);
}

bool JT_MessageCarbons::take(const QDomElement &e)
{
    if (iqVerify(e, Jid(), id())) {
        if (e.attribute(QString::fromLatin1("type")) != QString::fromLatin1("result"))
            setError(e);
        else
            setSuccess();
        return true;
    }
    return false;
}

//--------------------------------------------------
// class CarbonsSubscriber
//--------------------------------------------------

bool CarbonsSubscriber::xmlEvent(const QDomElement &root, QDomElement &e, Client *client, int userData, bool nested)
{
    bool drop = false;
    frw.setType(Forwarding::ForwardedNone);
    if (!nested) {
        Jid from(root.attribute(QStringLiteral("from")));
        Jid to(root.attribute(QStringLiteral("to")));
        if (from.resource().isEmpty() && from.compare(to, false)) {
            QDomElement child = e.firstChildElement();
            while (!child.isNull()) {
                if (frw.fromXml(child, client)) {
                    frw.setType(static_cast<Forwarding::Type>(userData));
                    break;
                }
                child = child.nextSiblingElement();
            }
        } else
            drop = true;
        e = QDomElement();
    }
    return drop;
}

bool CarbonsSubscriber::messageEvent(Message &msg, int userData, bool nested)
{
    Q_UNUSED(userData)
    if (!nested && frw.type() != Forwarding::ForwardedNone) {
        msg.setForwarded(frw);
        frw.setType(Forwarding::ForwardedNone);
    }
    return false;
}

//--------------------------------------------------
// class CarbonsManager
//--------------------------------------------------

class CarbonsManager::Private {
public:
    ~Private()
    {
        //        if (sbs.get())
        //            unsubscribe();
    }

    void subscribe()
    {
        push_m->subscribeXml(sbs.get(), QString::fromLatin1("received"), xmlns_carbons,
                             Forwarding::ForwardedCarbonsReceived);
        push_m->subscribeXml(sbs.get(), QString::fromLatin1("sent"), xmlns_carbons, Forwarding::ForwardedCarbonsSent);
        push_m->subscribeMessage(sbs.get(), 0);
    }

    void unsubscribe()
    {
        push_m->unsubscribeXml(sbs.get(), QString::fromLatin1("received"), xmlns_carbons);
        push_m->unsubscribeXml(sbs.get(), QString::fromLatin1("sent"), xmlns_carbons);
        push_m->unsubscribeMessage(sbs.get());
    }

    JT_PushMessage                    *push_m;
    std::unique_ptr<CarbonsSubscriber> sbs;
    bool                               enable = false;
};

CarbonsManager::CarbonsManager(JT_PushMessage *push_m) : QObject(push_m), d(new Private)
{
    d->push_m = push_m;
    d->sbs.reset(new CarbonsSubscriber());
}

CarbonsManager::~CarbonsManager() { }

QDomElement CarbonsManager::privateElement(QDomDocument &doc)
{
    return doc.createElementNS(xmlns_carbons, QString::fromLatin1("private"));
}

void CarbonsManager::setEnabled(bool enable)
{
    if (d->enable == enable)
        return;

    if (enable) {
        d->subscribe();
        JT_MessageCarbons *jt = new JT_MessageCarbons(d->push_m->client()->rootTask());
        connect(
            jt, &JT_MessageCarbons::finished, this,
            [this, jt]() {
                if (jt->success())
                    d->enable = true;
                else
                    d->unsubscribe();
                emit finished();
            },
            Qt::QueuedConnection);
        jt->enable();
        jt->go(true);
    } else {
        JT_MessageCarbons *jt = new JT_MessageCarbons(d->push_m->client()->rootTask());
        connect(
            jt, &JT_MessageCarbons::finished, this,
            [this]() {
                d->enable = false;
                d->unsubscribe();
                emit finished();
            },
            Qt::QueuedConnection);
        jt->disable();
        jt->go(true);
    }
}

bool CarbonsManager::isEnabled() const { return d->enable; }

} // namespace XMPP

#include "xmpp_carbons.moc"
