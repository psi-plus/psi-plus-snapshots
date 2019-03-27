/*
 * jignle.cpp - General purpose Jingle
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "jingle.h"
#include "xmpp_xmlcommon.h"
#include "xmpp/jid/jid.h"
#include "xmpp-im/xmpp_hash.h"
#include "xmpp_client.h"
#include "xmpp_task.h"
#include "xmpp_stream.h"

#include <QDateTime>
#include <QDomElement>
#include <QMap>
#include <QMap>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace XMPP {
namespace Jingle {

const QString NS(QStringLiteral("urn:xmpp:jingle:1"));


//----------------------------------------------------------------------------
// Jingle
//----------------------------------------------------------------------------
static const struct {
    const char *text;
    Action action;
} jingleActions[] = {
{ "content-accept",     Action::ContentAccept },
{ "content-add",        Action::ContentAdd },
{ "content-modify",     Action::ContentModify },
{ "content-reject",     Action::ContentReject },
{ "content-remove",     Action::ContentRemove },
{ "description-info",   Action::DescriptionInfo },
{ "security-info",      Action::SecurityInfo },
{ "session-accept",     Action::SessionAccept },
{ "session-info",       Action::SessionInfo },
{ "session-initiate",   Action::SessionInitiate },
{ "session-terminate",  Action::SessionTerminate },
{ "transport-accept",   Action::TransportAccept },
{ "transport-info",     Action::TransportInfo },
{ "transport-reject",   Action::TransportReject },
{ "transport-replace",  Action::TransportReplace }
};

class Jingle::Private : public QSharedData
{
public:
    Action action;
    QString sid;
    Jid initiator;
    Jid responder;
};

Jingle::Jingle()
{

}

Jingle::Jingle(Action action, const QString &sid) :
    d(new Private)
{
    d->action = action;
    d->sid = sid;
}

Jingle::Jingle(const QDomElement &e)
{
    QString actionStr = e.attribute(QLatin1String("action"));
    Action action;
    QString sid = e.attribute(QLatin1String("sid"));
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
    d->responder = responder;
}

Jingle::Jingle(const Jingle &other) :
    d(other.d)
{

}

Jingle::~Jingle()
{

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
    if (!d || d->sid.isEmpty() || d->action == Action::NoAction) {
        return QDomElement();
    }

    QDomElement query = doc->createElementNS(NS, QLatin1String("jingle"));
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

    return query;
}

Action Jingle::action() const
{
    return d->action;
}

const QString &Jingle::sid() const
{
    return d->sid;
}

const Jid &Jingle::initiator() const
{
    return d->initiator;
}

void Jingle::setInitiator(const Jid &jid)
{
    d->initiator = jid;
}

const Jid &Jingle::responder() const
{
    return d->responder;
}

void Jingle::setResponder(const Jid &jid)
{
    d->responder = jid;
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

Reason::Reason()
{

}

Reason::~Reason()
{

}

Reason::Reason(Reason::Condition cond) :
    d(new Private)
{
    d->cond = cond;
}

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

Reason::Reason(const Reason &other) :
    d(other.d)
{

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
// ContentBase
//----------------------------------------------------------------------------
ContentBase::ContentBase(Origin creator, const QString &name) :
    creator(creator), name(name)
{
}

ContentBase::ContentBase(const QDomElement &el)
{
    static QMap<QString,Origin> sendersMap({
                                                {QStringLiteral("initiator"), Origin::Initiator},
                                                {QStringLiteral("none"), Origin::Initiator},
                                                {QStringLiteral("responder"), Origin::Initiator}
                                            });
    creator = creatorAttr(el);
    name = el.attribute(QLatin1String("name"));
    senders = sendersMap.value(el.attribute(QLatin1String("senders")));
    disposition = el.attribute(QLatin1String("disposition")); // if empty, it's "session"
}

QDomElement ContentBase::toXml(QDomDocument *doc, const char *tagName) const
{
    if (!isValid()) {
        return QDomElement();
    }
    auto el = doc->createElement(QLatin1String(tagName));
    setCreatorAttr(el, creator);
    el.setAttribute(QLatin1String("name"), name);

    QString sendersStr;
    switch (senders) {
    case Origin::None:
        sendersStr = QLatin1String("none");
        break;

    case Origin::Initiator:
        sendersStr = QLatin1String("initiator");
        break;

    case Origin::Responder:
        sendersStr = QLatin1String("responder");
        break;

    case Origin::Both:
    default:
        break;
    }

    if (!disposition.isEmpty() && disposition != QLatin1String("session")) {
        el.setAttribute(QLatin1String("disposition"), disposition); // NOTE review how we can parse it some generic way
    }
    if (!sendersStr.isEmpty()) {
        el.setAttribute(QLatin1String("senders"), sendersStr);
    }

    return el;
}


Origin ContentBase::creatorAttr(const QDomElement &el)
{
    auto creatorStr = el.attribute(QLatin1String("creator"));
    if (creatorStr == QLatin1String("initiator")) {
        return Origin::Initiator;
    }
    if (creatorStr == QLatin1String("responder")) {
        return Origin::Responder;
    }
    return Origin::None;
}

bool ContentBase::setCreatorAttr(QDomElement &el, Origin creator)
{
    if (creator == Origin::Initiator) {
        el.setAttribute(QLatin1String("creator"), QLatin1String("initiator"));
    } else if (creator == Origin::Responder) {
        el.setAttribute(QLatin1String("creator"), QLatin1String("responder"));
    } else {
        return false;
    }
    return true;
}

//----------------------------------------------------------------------------
// Application
//----------------------------------------------------------------------------
ApplicationManager::ApplicationManager(QObject *parent) :
    QObject(parent)
{
}

//----------------------------------------------------------------------------
// TransportManager
//----------------------------------------------------------------------------
TransportManager::TransportManager(QObject *parent) :
    QObject(parent)
{

}

//----------------------------------------------------------------------------
// JT - Jingle Task
//----------------------------------------------------------------------------
class JTPush : public Task
{
    Q_OBJECT
public:
    JTPush(Task *parent) :
        Task(parent)
    {

    }

    ~JTPush(){}

    bool take(const QDomElement &iq)
    {
        if (iq.tagName() != QLatin1String("iq") || iq.attribute(QLatin1String("type")) != QLatin1String("set")) {
            return false;
        }
        auto jingleEl = iq.firstChildElement(QStringLiteral("jingle"));
        if (jingleEl.isNull() || jingleEl.namespaceURI() != ::XMPP::Jingle::NS) {
            return false;
        }
        Jingle jingle(jingleEl);
        if (!jingle.isValid()) {
            respondError(iq, Stanza::Error::Cancel, Stanza::Error::BadRequest);
            return true;
        }

        QString fromStr(iq.attribute(QStringLiteral("from")));
        Jid from(fromStr);
        if (jingle.action() == Action::SessionInitiate) {
            if (!client()->jingleManager()->isAllowedParty(from) ||
                    (!jingle.initiator().isEmpty() && !client()->jingleManager()->isAllowedParty(jingle.initiator()))) {
                respondError(iq, Stanza::Error::Cancel, Stanza::Error::ServiceUnavailable);
                return true;
            }

            Jid redirection(client()->jingleManager()->redirectionJid());
            if (redirection.isValid()) {
                respondError(iq, Stanza::Error::Modify, Stanza::Error::Redirect, QStringLiteral("xmpp:")+redirection.full());
                return true;
            }

            auto session = client()->jingleManager()->session(from, jingle.sid());
            if (session) {
                // FIXME what if not yet acknowledged. xep-0166 has a solution for that
                respondError(iq, Stanza::Error::Cancel, Stanza::Error::Conflict);
                return true;
            }
            session = client()->jingleManager()->incomingSessionInitiate(from, jingle, jingleEl);
            if (!session) {
                respondError(iq, client()->jingleManager()->lastError());
                return true;
            }
        } else {
            auto session = client()->jingleManager()->session(from, jingle.sid());
            if (session) {
                respondError(iq, Stanza::Error::Cancel, Stanza::Error::Conflict);
                return true;
            }
            if (!session->updateFromXml(jingle.action(), jingleEl)) {
                respondError(iq, session->lastError());
                return true;
            }
        }

        auto resp = createIQ(client()->doc(), "result", fromStr, iq.attribute(QStringLiteral("id")));
        client()->send(resp);
        return true;
    }

    void respondError(const QDomElement &iq, Stanza::Error::ErrorType errType, Stanza::Error::ErrorCond errCond, const QString &text = QString())
    {
        auto resp = createIQ(client()->doc(), "error", iq.attribute(QStringLiteral("from")), iq.attribute(QStringLiteral("id")));
        Stanza::Error error(errType, errCond, text);
        resp.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
        client()->send(resp);
    }

    void respondError(const QDomElement &iq, const Stanza::Error &error)
    {
        auto resp = createIQ(client()->doc(), "error", iq.attribute(QStringLiteral("from")), iq.attribute(QStringLiteral("id")));
        resp.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
        client()->send(resp);
    }
};

//----------------------------------------------------------------------------
// JT - Jingle Task
//----------------------------------------------------------------------------
class JT : public Task
{
    Q_OBJECT

    QDomElement iq_;
    Jid to_;

public:
    JT(Task *parent) :
        Task(parent)
    {

    }

    ~JT(){}

    void request(const Jid &to, const QDomElement &jingleEl)
    {
        to_ = to;
        iq_ = createIQ(doc(), "set", to.full(), id());
        iq_.appendChild(jingleEl);
    }

    void onGo()
    {
        send(iq_);
    }

    bool take(const QDomElement &x)
    {
        if(!iqVerify(x, to_, id()))
            return false;

        if(x.attribute("type") == "error") {
            setError(x);
        } else {
            setSuccess();
        }
        return true;
    }
};


//----------------------------------------------------------------------------
// Session
//----------------------------------------------------------------------------
class Session::Private
{
public:
    Session *q;
    Manager *manager;
    QTimer stepTimer;
    Session::State state = Session::Starting;
    Origin  role  = Origin::Initiator; // my role in the session
    XMPP::Stanza::Error lastError;
    Action outgoingUpdateType = Action::NoAction;
    QDomElement outgoingUpdate;
    QMap<QString,QWeakPointer<ApplicationManagerPad>> applicationPads;
    QMap<QString,QWeakPointer<TransportManagerPad>> transportPads;
    QMap<QString,Application*> myContent;     // content::creator=(role == Session::Role::Initiator?initiator:responder)
    QMap<QString,Application*> remoteContent; // content::creator=(role == Session::Role::Responder?initiator:responder)
    QSet<Application*> signalingContent;
    QString sid;
    Jid origFrom; // "from" attr of IQ.
    Jid otherParty; // either "from" or initiator/responder. it's where to send all requests.
    Jid localParty; // that one will be set as initiator/responder if provided
    bool waitingAck = false;

    void sendJingle(Action action, QList<QDomElement> update)
    {
        QDomDocument &doc = *manager->client()->doc();
        Jingle jingle(action, sid);
        if (action == Action::SessionInitiate) {
            jingle.setInitiator(manager->client()->jid());
        }
        if (action == Action::SessionAccept) {
            jingle.setResponder(manager->client()->jid());
        }
        auto xml = jingle.toXml(&doc);

        for (const QDomElement &e: update) {
            xml.appendChild(e);
        }

        auto jt = new JT(manager->client()->rootTask());
        jt->request(otherParty, xml);
        QObject::connect(jt, &JT::finished, manager, [jt, jingle, this](){
            waitingAck = false;
            // TODO handle errors
            planStep();
        });
        waitingAck = true;
        jt->go(true);
    }

    void planStep() {
        if (waitingAck) {
            return;
        }
        lastError = Stanza::Error(0, 0);
        if (!stepTimer.isActive()) {
            stepTimer.start();
        }
    }

    // come here from doStep. in other words it's safe to not check current state.
    void sendSessionInitiate()
    {
        waitingAck = true;
        state = Session::Unacked;
        auto jt = new JT(manager->client()->rootTask());

        Jingle jingle(Action::SessionInitiate, manager->generateSessionId(otherParty));
        Jid initiator  = localParty.isValid()? localParty : manager->client()->jid();
        jingle.setInitiator(initiator);
        QDomElement jingleEl = jingle.toXml(manager->client()->doc());

        for (const auto &p: myContent) {
            jingleEl.appendChild(p->takeOutgoingUpdate());
        }
        jt->request(otherParty, jingleEl);
        jt->connect(jt, &JT::finished, q, [this, jt](){
            waitingAck = false;
            if (jt->success()) {
                state = Session::Pending;
                planStep();
            } else {
                state = Session::Ended;
                lastError = jt->error();
                emit q->terminated();
                q->deleteLater();
            }
        });
        jt->go(true);
    }

    void doStep() {
        if (waitingAck) { // we will return here when ack is received
            return;
        }
        if (state == Session::Starting) {
            return; // we will start doing something when initiate() is called
        }
        if (state == Session::WaitInitiateReady) {
            // we have if all the content is ready for initiate().
            for (const auto &c: myContent) {
                auto out = c->outgoingUpdateType();
                if (out == Action::ContentReject) { // yeah we are rejecting local content. invalid?
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    state = Session::Ended;
                    q->deleteLater();
                    emit q->terminated();
                    return;
                }
                if (out != Action::ContentAdd) {
                    return; // keep waiting.
                }
            }
            // so all contents is ready for session-initiate. let's do it
            sendSessionInitiate();
        }
        QList<QDomElement> updateXml;
        for (auto mp : applicationPads) {
            auto p = mp.toStrongRef();
            QDomElement el = p->takeOutgoingSessionInfoUpdate();
            if (!el.isNull()) {
                updateXml.append(el);
                // we can send session-info for just one application. so stop processing
                sendJingle(Action::SessionInfo, updateXml);
                return;
            }
        }

        bool needCheckAcceptance = (role == Origin::Responder && state != Session::State::Active);
        bool needAccept = needCheckAcceptance;
        QMultiMap<Action, Application*> updates;

        for (auto app : myContent) {
            Action updateType = app->outgoingUpdateType();
            if (updateType != Action::NoAction) {
                updates.insert(updateType, app);
                needAccept = false;
            } else if (needCheckAcceptance && !app->isReadyForSessionAccept()) {
                needAccept = false;
            }
        }

        // the same for remote. where is boost::join in Qt?
        for (auto app : remoteContent) {
            Action updateType = app->outgoingUpdateType();
            if (updateType != Action::NoAction) {
                updates.insert(updateType, app);
                needAccept = false;
            } else if (needCheckAcceptance && !app->isReadyForSessionAccept()) {
                needAccept = false;
            }
        }

        if (updates.size()) {
            Action action = updates.begin().key();
            auto apps = updates.values(action);
            for (auto app: apps) {
                updateXml.append(app->takeOutgoingUpdate());
            }
            sendJingle(action, updateXml);
        } else if (needAccept) {
            for (auto app : myContent) {
                updateXml.append(app->sessionAcceptContent());
            }
            for (auto app : remoteContent) {
                updateXml.append(app->sessionAcceptContent());
            }
            sendJingle(Action::SessionAccept, updateXml);
        }
    }

    Reason reason(const QDomElement &jingleEl)
    {
        QDomElement re = jingleEl.firstChildElement(QLatin1String("reason"));
        Reason reason;
        if(!re.isNull()) {
            reason = Reason(re);
            if (!reason.isValid()) {
                qDebug("invalid reason");
            }
        }
        return reason;
    }

    enum AddContentError {
        Ok,
        Unparsed,
        Unsupported
    };

    std::tuple<AddContentError, Reason::Condition, Application*> addContent(const QDomElement &ce)
    {
        QDomElement descriptionEl = ce.firstChildElement(QLatin1String("description"));
        QDomElement transportEl = ce.firstChildElement(QLatin1String("transport"));
        QString descriptionNS = descriptionEl.namespaceURI();
        QString transportNS = transportEl.namespaceURI();
        typedef std::tuple<AddContentError, Reason::Condition, Application*> result;

        ContentBase c(ce);
        if (!c.isValid() || descriptionEl.isNull() || transportEl.isNull() || descriptionNS.isEmpty() || transportNS.isEmpty()) {
            return result{Unparsed, Reason::Success, nullptr};
        }

        auto appPad = q->applicationPadFactory(descriptionNS);
        if (!appPad) {
            return result{Unsupported, Reason::UnsupportedApplications, nullptr}; // <unsupported-applications/> condition
        }
        QScopedPointer<Application> app(appPad->manager()->startApplication(appPad, c.name, c.creator, c.senders));
        auto descErr = app->setDescription(descriptionEl);
        if (descErr == Application::IncompatibleParameters) {
            return result{Unsupported, Reason::IncompatibleParameters, nullptr};
        } else
        if (descErr == Application::Unparsed) {
            return result{Unparsed, Reason::Success, nullptr};
        } else
        {
            // same for transport
            auto trPad = q->transportPadFactory(transportNS);
            if (!trPad) {
                return result{Unsupported, Reason::UnsupportedTransports, app.take()}; // <unsupported-transports/> condition or we can try fallback and fail with <failed-transport/>
            }
            auto transport = trPad->manager()->newTransport(trPad, transportEl);
            if (transport) {
                if (app->setTransport(transport)) {
                    return result{Ok, Reason::Success, app.take()};
                }
                return result{Unsupported, Reason::UnsupportedTransports, app.take()};
            }
        }

        return result{Unparsed, Reason::Success, nullptr};
    }
};

Session::Session(Manager *manager, const Jid &peer) :
    d(new Private)
{
    d->q = this;
    d->manager = manager;
    d->otherParty = peer;
    d->stepTimer.setSingleShot(true);
    d->stepTimer.setInterval(0);
    connect(&d->stepTimer, &QTimer::timeout, this, [this](){ d->doStep();});

}

Session::~Session()
{

}

Manager *Session::manager() const
{
    return d->manager;
}

Session::State Session::state() const
{
    return d->state;
}

Jid Session::me() const
{
    return d->manager->client()->jid();
}

Jid Session::peer() const
{
    return d->otherParty;
}

Jid Session::initiator() const
{
    return d->role == Origin::Initiator? d->manager->client()->jid() : d->otherParty;
}

Jid Session::responder() const
{
    return d->role == Origin::Responder? d->manager->client()->jid() : d->otherParty;
}

Origin Session::role() const
{
    return d->role;
}

XMPP::Stanza::Error Session::lastError() const
{
    return d->lastError;
}

Application *Session::newContent(const QString &ns, Origin senders)
{
    auto pad = applicationPadFactory(ns);
    if (pad) {
        return pad->manager()->startApplication(pad, pad->generateContentName(senders), d->role, senders);
    }
    return nullptr;
}

Application *Session::content(const QString &contentName, Origin creator)
{
    if (creator == d->role) {
        return d->myContent.value(contentName);
    } else {
        return d->remoteContent.value(contentName);
    }
}

void Session::addContent(Application *content)
{
    d->myContent.insert(content->contentName(), content);
    if (d->state != Session::Starting && content->outgoingUpdateType() != Action::NoAction) {
        d->signalingContent.insert(content);
    }
    connect(content, &Application::updated, this, [this](){
        d->signalingContent.insert(static_cast<Application*>(sender()));
        if (!d->waitingAck && !d->stepTimer.isActive()) {
            d->stepTimer.start();
        }
    });
    /**
      we have d->pendingSendLocalContent,    // before it's sent to remote side
              d->pendingAckLocalContent,     // not necessary. use lambda capture
              d->pendingConfirmLocalContent, // send to remote side but no iq ack yet
              d->localContent                // accepted with session-accept or content-accept
      First we add to pendingSendLocalContent.
      Connect signals to the content, like
        - updated
      If we are waiting for ack from remote, then just exit. We will get back to this later
      If session->state is Created, then just exit. We will get back to this when session->initiate is called
      So session was already initiated and it's a regular content add. We don't wait anythig so have to send
      cotent-add request now.
      call session->d->flushContentAdd() to send it.

      flushContentAdd() iterates over all pendingSendLocalContent,
         if content->outgoingUpdateType() == Action::ContentAdd then it's ready to be sent,
            add it to temporary send list and remove from pendingSendLocalContent
      if temporary send list is not empty then append it to pendingAckLocalContent and send content-add

      On ack receive it will be decided what to do with pendingAckLocalContent. On iq timeout it will be put back
      to pendingSendLocalContent with consequent call to flushContentAdd().
      or on success it will added to pendingConfirmLocalContent
     */
}

ApplicationManagerPad::Ptr Session::applicationPad(const QString &ns)
{
    return d->applicationPads.value(ns).toStrongRef();
}

TransportManagerPad::Ptr Session::transportPad(const QString &ns)
{
    return d->transportPads.value(ns).toStrongRef();
}

QSharedPointer<Transport> Session::newOutgoingTransport(const QString &ns)
{
    auto pad = transportPadFactory(ns);
    if (pad) {
        return pad->manager()->newTransport(pad); // pad on both side becaue we need shared pointer
    }
    return QSharedPointer<Transport>();
}

QString Session::preferredApplication() const
{
    // TODO some heuristics to detect preferred application
    return QString();
}

QStringList Session::allApplicationTypes() const
{
    return d->applicationPads.keys();
}

void Session::setLocalJid(const Jid &jid)
{
    d->localParty = jid;
}

void Session::initiate()
{
    d->state = WaitInitiateReady;
    for (auto &c: d->myContent) {
        c->prepare();
    }
    d->planStep();
}

void Session::reject()
{
    // TODO
}

TransportManagerPad::Ptr Session::transportPadFactory(const QString &ns)
{
    auto pad = d->transportPads.value(ns).toStrongRef();
    if (!pad) {
        auto deleter = [ns, this](TransportManagerPad *pad){
            d->transportPads.remove(ns);
            delete pad;
        };
        pad = TransportManagerPad::Ptr(d->manager->transportPad(this, ns), deleter);
        if (pad) {
            d->transportPads.insert(ns, pad);
        }
    }
    return pad;
}

ApplicationManagerPad::Ptr Session::applicationPadFactory(const QString &ns)
{
    auto pad = d->applicationPads.value(ns).toStrongRef();
    if (!pad) {
        auto deleter = [ns, this](ApplicationManagerPad *pad){
            d->applicationPads.remove(ns);
            delete pad;
        };
        pad = ApplicationManagerPad::Ptr(d->manager->applicationPad(this, ns), deleter);
        if (pad) {
            d->applicationPads.insert(ns, pad);
        }
    }
    return pad;
}

bool Session::incomingInitiate(const Jingle &jingle, const QDomElement &jingleEl)
{
    d->sid = jingle.sid();
    d->origFrom = d->otherParty;
    if (jingle.initiator().isValid() && !jingle.initiator().compare(d->origFrom)) {
        d->otherParty = jingle.initiator();
    }
    //auto key = qMakePair(from, jingle.sid());

    QMap<QString,std::tuple<Reason::Condition, Application *>> addSet; // application to supported

    QString contentTag(QStringLiteral("content"));
    for(QDomElement ce = jingleEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        Private::AddContentError err;
        Reason::Condition cond;
        Application *app;
        auto r = d->addContent(ce);
        std::tie(err, cond, app) = r;
        if (err == Private::AddContentError::Unparsed) {
            for (auto const &i: addSet) {
                delete std::get<1>(i);
            }
            d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
            return false;
        }
        if (err == Private::AddContentError::Unsupported) {
            if (cond == Reason::Condition::UnsupportedTransports) {

            }
            // can't continue as well
            for (auto const &i: addSet) {
                delete std::get<1>(i);
            }
            d->outgoingUpdateType = Action::SessionTerminate;
            Reason r(cond);
            d->outgoingUpdate = r.toXml(d->manager->client()->doc());
            return true;
        }
        QString contentName = ce.attribute(QStringLiteral("name"));
        if (err == Private::AddContentError::Ok) {
            auto et = addSet.value(contentName);
            auto eapp = std::get<1>(et);
            if (!eapp || eapp->wantBetterTransport(app->transport())) {
                addSet.insert(contentName, std::make_tuple(cond,app));
            }
        } else if (!std::get<1>(addSet.value(contentName))) {
            // something unsupported. but lets parse all items. maybe it will get replaced
            addSet.insert(contentName, std::make_tuple(cond,app));
        }

        // TODO at this point if all addSet items have application it's success,
        // otherwise we have to think what to do with this. for example replace transport if it's unsupported.

        for (auto const &i: addSet) {
            Reason::Condition cond;
            Application *app;
            std::tie(cond, app) = i;
            if (!app) {
                // TODO
                return false; // FIXME. memory release
            }
            d->remoteContent.insert(app->contentName(), app);
        }
    }

    d->planStep();
    return true;
}

bool Session::updateFromXml(Action action, const QDomElement &jingleEl)
{
    if (action == Action::SessionInfo) {

    }

    if (action != Action::ContentAdd) {
        return false;
    }

    QMap<QString,Application *> addSet; // application to supported
    bool parsed = true;
    int unsupported = 0;
    Reason::Condition rejectCond = Reason::Condition::Success;

    QString contentTag(QStringLiteral("content"));
    for(QDomElement ce = jingleEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        Private::AddContentError err;
        Reason::Condition cond;
        Application *app;
        QString contentName = ce.attribute(QStringLiteral("name"));
        if (!contentName.size()) {
            parsed = false;
            break;
        }

        std::tie(err, cond, app) = d->addContent(ce);
        bool alreadyAdded = addSet.contains(contentName);
        if (err != Private::AddContentError::Ok) {
            // can't continue as well
            if (app) { // we are going to reject it completely so delete
                delete app;
            }
            if (err == Private::AddContentError::Unsupported) {
                rejectCond = cond;
            }
            if (!alreadyAdded) {
                unsupported++;
                addSet.insert(contentName, nullptr);
            } // else just ignore this unsupported content. we aready have one
            continue;
        }

        auto eapp = addSet.value(contentName);
        if (alreadyAdded && !eapp) {
            unsupported--; // we are going to overwrite previous with successful
        }
        if (!eapp || eapp->wantBetterTransport(app->transport())) {
            addSet.insert(contentName, app);
        }
    }

    if (unsupported && rejectCond == Reason::Condition::Success) {
        parsed = false; // the only way it's possible
    }
    if (!parsed) {
        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
        qDeleteAll(addSet);
        return false;
    } else if (unsupported) {
        d->outgoingUpdateType = Action::ContentReject;
        Reason r(rejectCond);
        d->outgoingUpdate = r.toXml(d->manager->client()->doc());
        qDeleteAll(addSet);
        return true;
    }

    for (auto const &app: addSet) {
        d->remoteContent.insert(app->contentName(), app); // TODO check conflicts
    }

    return true;
}


//----------------------------------------------------------------------------
// SessionManagerPad - handle event related to a type of app/transport but not specific instance
//----------------------------------------------------------------------------
QDomElement SessionManagerPad::takeOutgoingSessionInfoUpdate()
{
    return QDomElement();
}

//----------------------------------------------------------------------------
// Manager
//----------------------------------------------------------------------------
class Manager::Private
{
public:
    XMPP::Client *client;
    Manager *manager;
    QScopedPointer<JTPush> pushTask;
    // ns -> application
    QMap<QString,QPointer<ApplicationManager>> applicationManagers;
    // ns -> parser function
    QMap<QString,QPointer<TransportManager>> transportManagers;
    std::function<bool(const Jid &)> remoteJidCecker;

    // when set/valid any incoming session initiate will be replied with redirection error
    Jid redirectionJid;
    XMPP::Stanza::Error lastError;
    QHash<QPair<Jid,QString>,Session*> sessions;
    int maxSessions = -1; // no limit
};

Manager::Manager(Client *client) :
    d(new Private())
{
    d->client = client;
    d->manager = this;
    d->pushTask.reset(new JTPush(client->rootTask()));
}

Manager::~Manager()
{
}

Client *Manager::client() const
{
    return d->client;
}

void Manager::setRedirection(const Jid &to)
{
    d->redirectionJid = to;
}

const Jid &Manager::redirectionJid() const
{
    return d->redirectionJid;
}

void Manager::registerApp(const QString &ns, ApplicationManager *app)
{
    d->applicationManagers.insert(ns, app);
    app->setJingleManager(this);
}

void Manager::unregisterApp(const QString &ns)
{
    auto appManager = d->applicationManagers.value(ns);
    if (appManager) {
        appManager->closeAll();
        d->applicationManagers.remove(ns);
    }
}

bool Manager::isRegisteredApplication(const QString &ns)
{
    return d->applicationManagers.contains(ns);
}

ApplicationManagerPad *Manager::applicationPad(Session *session, const QString &ns)
{
    auto am = d->applicationManagers.value(ns);
    if (!am) {
        return NULL;
    }
    return am->pad(session);
}

void Manager::registerTransport(const QString &ns, TransportManager *transport)
{
    d->transportManagers.insert(ns, transport);
    transport->setJingleManager(this);
}

void Manager::unregisterTransport(const QString &ns)
{
    auto trManager = d->transportManagers.value(ns);
    if (trManager) {
        trManager->closeAll();
        d->transportManagers.remove(ns);
    }
}

bool Manager::isRegisteredTransport(const QString &ns)
{
    return d->transportManagers.contains(ns);
}

bool Manager::isAllowedParty(const Jid &jid) const
{
    if (d->remoteJidCecker) {
        return d->remoteJidCecker(jid);
    }
    // REVIEW probably we can check Client's internal roster when checker is not set.
    return true;
}

Session *Manager::session(const Jid &remoteJid, const QString &sid)
{
    return d->sessions.value(qMakePair(remoteJid, sid));
}

void Manager::setRemoteJidChecker(std::function<bool(const Jid &)> checker)
{
    d->remoteJidCecker = checker;
}

TransportManagerPad* Manager::transportPad(Session *session, const QString &ns)
{
    auto transportManager = d->transportManagers.value(ns);
    if (!transportManager) {
        return NULL;
    }
    return transportManager->pad(session);
}

QStringList Manager::availableTransports(const Transport::Features &features) const
{
    QStringList ret;
    for (auto it = d->transportManagers.cbegin(); it != d->transportManagers.cend(); ++it) {
        if (((*it)->features() & features) == features) {
            ret.append(it.key());
        }
    }
    return ret;
}

Session* Manager::incomingSessionInitiate(const Jid &from, const Jingle &jingle, const QDomElement &jingleEl)
{
    if (d->maxSessions > 0 && d->sessions.size() == d->maxSessions) {
        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Wait, XMPP::Stanza::Error::ResourceConstraint);
        return NULL;
    }
    auto key = qMakePair(from, jingle.sid());
    auto s = new Session(this, from);
    if (s->incomingInitiate(jingle, jingleEl)) { // if parsed well
        d->sessions.insert(key, s);
        // emit incomingSession makes sense when there are no unsolved conflicts in content descriptions / transports
        // QMetaObject::invokeMethod(this, "incomingSession", Qt::QueuedConnection, Q_ARG(Session*, s));
        return s;
    }
    d->lastError = s->lastError();
    delete s;
    return NULL;
}

XMPP::Stanza::Error Manager::lastError() const
{
    return d->lastError;
}

Session *Manager::newSession(const Jid &j)
{
    return new Session(this, j);
}

QString Manager::generateSessionId(const Jid &peer)
{
    QString id;
    do {
        id = QString("%1").arg(quint32(qrand()), 6, 32, QChar('0'));
    } while (d->sessions.contains(QPair<Jid,QString>(peer,id)));
    return id;
}

Origin negateOrigin(Origin o)
{
    switch (o) {
    case Origin::None:      return Origin::Both;
    case Origin::Both:      return Origin::None;
    case Origin::Initiator: return Origin::Responder;
    case Origin::Responder: return Origin::Initiator;
    }
    return Origin::None;
}


} // namespace Jingle
} // namespace XMPP

#include "jingle.moc"
