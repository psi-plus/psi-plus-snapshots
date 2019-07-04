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
#include <QDebug>
#include <QTimer>

namespace XMPP {
namespace Jingle {

const QString NS(QStringLiteral("urn:xmpp:jingle:1"));
const QString ERROR_NS(QStringLiteral("urn:xmpp:jingle:errors:1"));


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

Reason::Reason(Reason::Condition cond, const QString &text) :
    d(new Private)
{
    d->cond = cond;
    d->text = text;
}

Reason::Reason(const QDomElement &e)
{
    if(e.tagName() != QLatin1String("reason"))
        return;

    Condition condition = NoReason;
    QString text;
    QString rns = e.namespaceURI();

    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        if (c.tagName() == QLatin1String("text")) {
            text = c.text();
        }
        else if (c.namespaceURI() != rns) {
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

Reason &Reason::operator=(const Reason &other)
{
    d = other.d;
    return *this;
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
// JTPush - Jingle Task
//----------------------------------------------------------------------------
class JTPush : public Task
{
    Q_OBJECT

    QList<QString> externalManagers;
    QList<QString> externalSessions;
public:
    JTPush(Task *parent) :
        Task(parent)
    {

    }

    ~JTPush(){}

    inline void addExternalManager(const QString &ns) { externalManagers.append(ns); }
    inline void forgetExternalSession(const QString &sid) { externalSessions.removeOne(sid); }


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

        if (externalManagers.size()) {
            if (jingle.action() == Action::SessionInitiate) {
                auto cname = QString::fromLatin1("content");
                auto dname = QString::fromLatin1("description");
                for (auto n = jingleEl.firstChildElement(cname); !n.isNull(); n = n.nextSiblingElement(cname)) {
                    auto del = n.firstChildElement(dname);
                    if (!del.isNull() && externalManagers.contains(del.namespaceURI())) {
                        externalSessions.append(jingle.sid());
                        return false;
                    }
                }
            } else if (externalSessions.contains(jingle.sid())) {
                if (jingle.action() == Action::SessionTerminate) {
                    externalSessions.removeOne(jingle.sid());
                }
                return false;
            }
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
                if (session->role() == Origin::Initiator) { //
                    respondTieBreak(iq);
                } else {
                    // second session from this peer with the same sid.
                    respondError(iq, Stanza::Error::Cancel, Stanza::Error::BadRequest);
                }
                return true;
            }
            session = client()->jingleManager()->incomingSessionInitiate(from, jingle, jingleEl);
            if (!session) {
                respondError(iq, client()->jingleManager()->lastError());
                return true;
            }
        } else {
            auto session = client()->jingleManager()->session(from, jingle.sid());
            if (!session) {
                if (jingle.action() == Action::SessionTerminate) {
                    auto resp = createIQ(client()->doc(), "result", fromStr, iq.attribute(QStringLiteral("id")));
                    client()->send(resp);
                } else {
                    auto el = client()->doc()->createElementNS(ERROR_NS, QStringLiteral("unknown-session"));
                    respondError(iq, Stanza::Error::Cancel, Stanza::Error::ItemNotFound, QString(), el);
                }
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

    void respondError(const QDomElement &iq, Stanza::Error::ErrorType errType, Stanza::Error::ErrorCond errCond,
                      const QString &text = QString(), const QDomElement &jingleErr = QDomElement())
    {
        auto resp = createIQ(client()->doc(), "error", iq.attribute(QStringLiteral("from")), iq.attribute(QStringLiteral("id")));
        Stanza::Error error(errType, errCond, text);
        auto errEl = error.toXml(*client()->doc(), client()->stream().baseNS());
        if (!jingleErr.isNull()) {
            errEl.appendChild(jingleErr);
        }
        resp.appendChild(errEl);
        client()->send(resp);
    }

    void respondTieBreak(const QDomElement &iq)
    {
        Stanza::Error error(Stanza::Error::Cancel, Stanza::Error::Conflict);
        ErrorUtil::fill(*client()->doc(), error, ErrorUtil::TieBreak);
        respondError(iq, error);
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
    State state = State::Created; // state of session on our side. if it's incoming we start from Created anyaway but Pending state is skipped
    Origin  role  = Origin::Initiator; // my role in the session
    XMPP::Stanza::Error lastError;
    Reason terminateReason;
    QMap<QString,QWeakPointer<ApplicationManagerPad>> applicationPads;
    QMap<QString,QWeakPointer<TransportManagerPad>> transportPads;
    //QMap<QString,Application*> myContent;     // content::creator=(role == Session::Role::Initiator?initiator:responder)
    //QMap<QString,Application*> remoteContent; // content::creator=(role == Session::Role::Responder?initiator:responder)
    QMap<ContentKey,Application*> contentList;
    QSet<Application*> signalingContent;
    QHash<Action,OutgoingUpdate> outgoingUpdates; // session level updates. session-info for example or some rejected apps
    QString sid;
    Jid origFrom; // "from" attr of IQ.
    Jid otherParty; // either "from" or initiator/responder. it's where to send all requests.
    Jid localParty; // that one will be set as initiator/responder if provided
    bool waitingAck = false;

    void setSessionFinished()
    {
        state = State::Finished;
        emit q->terminated();
        signalingContent.clear();
        for (auto &c: contentList) {
            if (c->state() != State::Finished) {
                c->setState(State::Finished);
            }
        }
        auto vals = contentList.values();
        contentList.clear();
        while (vals.size()) {
            vals.takeLast()->deleteLater();
        }
        q->deleteLater();
    }

    void sendJingle(Action action, QList<QDomElement> update, std::function<void()> successCB = std::function<void()>())
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
        QObject::connect(jt, &JT::finished, q, [jt, jingle, successCB, this](){
            waitingAck = false;
            if (jt->success()) {
                if (successCB) {
                    successCB();
                } else {
                    planStep();
                }
            } else {
                lastError = jt->error();
                if (ErrorUtil::jingleCondition(lastError) != ErrorUtil::TieBreak)
                    setSessionFinished();
                else
                    planStep();
            }
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

    void doStep() {
        if (waitingAck) { // we will return here when ack is received. Session::Unacked is possible also only with waitingAck
            return;
        }
        if (terminateReason.condition() && state != State::Finished) {
            if (state != State::Created || role == Origin::Responder) {
                sendJingle(Action::SessionTerminate, QList<QDomElement>() << terminateReason.toXml(manager->client()->doc()));
            }
            setSessionFinished();
            return;
        }
        if (state == State::Created || state == State::Finished) {
            return; // we will start doing something when initiate() is called
        }

        if (outgoingUpdates.size()) {
            auto it = outgoingUpdates.begin();
            auto action = it.key();
            auto updates = it.value();
            auto elements = std::get<0>(updates);
            auto cb = std::get<1>(updates);
            outgoingUpdates.erase(it);
            sendJingle(action, elements, [this, cb](){
                if (cb) {
                    cb();
                }
                planStep();
            });
            return;
        }

        typedef std::tuple<QPointer<Application>,OutgoingUpdateCB> AckHndl; // will be used from callback on iq ack
        if (state == State::PrepareLocalOffer) { // we are going to send session-initiate/accept (already accepted by the user but not sent yet)
            /*
             * For session-initiate everything is prety much straightforward, just any content with Action::ContentAdd
             * update type has to be added. But with session-accept things are more complicated
             *   1. Local client could add its content. So we have to check content origin too.
             *   2. Remote client could add more content before local session-accept. Then we have two options
             *         a) send content-accept and skip this content in session-accept later
             *         b) don't send content-accept and accept everything with session-accept
             *      We prefer option (b) in our implementation.
             */
            Action expectedContentAction = role == Origin::Initiator? Action::ContentAdd : Action::ContentAccept;
            for (const auto &c: contentList) {
                auto out = c->evaluateOutgoingUpdate();
                if (out == Action::ContentReject) { // yeah we are rejecting local content. invalid?
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    setSessionFinished();
                    return;
                }
                if (out != expectedContentAction) {
                    return; // keep waiting.
                }
            }
            Action actionToSend = Action::SessionAccept;
            State finalState = State::Active;
            // so all contents is ready for session-initiate. let's do it
            if (role == Origin::Initiator) {
                sid = manager->registerSession(q);
                actionToSend = Action::SessionInitiate;
                finalState = State::Pending;
            }

            QList<QDomElement> contents;
            QList<AckHndl> acceptApps;
            for (const auto &p: contentList) {
                QList<QDomElement> xml;
                OutgoingUpdateCB callback;
                std::tie(xml, callback) = p->takeOutgoingUpdate();
                contents += xml;
                //p->setState(State::Unacked);
                if (callback) {
                    acceptApps.append(AckHndl{p, callback});
                }
            }
            state = State::Unacked;
            sendJingle(actionToSend, contents, [this, acceptApps, finalState](){
                state = finalState;
                for (const auto &h: acceptApps) {
                    auto app = std::get<0>(h);
                    auto callback = std::get<1>(h);
                    if (app) {
                        callback();
                        if (role == Origin::Responder) {
                            app->start();
                        }
                    }
                }
                if (finalState == State::Active) {
                    emit q->activated();
                }
                planStep();
            });

            return;
        }

        // So session is either in State::Pending or State::Active here.
        // State::Connecting status is skipped for session.
        QList<QDomElement> updateXml;
        for (auto mp : applicationPads) {
            auto p = mp.toStrongRef();
            QDomElement el = p->takeOutgoingSessionInfoUpdate();
            if (!el.isNull()) {
                updateXml.append(el);
                // we can send session-info for just one application. so stop processing
                sendJingle(Action::SessionInfo, updateXml, [this](){planStep();});
                return;
            }
        }

        QMultiMap<Action, Application*> updates;

        for (auto app : signalingContent) {
            Action updateType = app->evaluateOutgoingUpdate();
            if (updateType != Action::NoAction) {
                updates.insert(updateType, app);
            }
        }

        QList<AckHndl> acceptApps;
        if (updates.size()) {
            Action action = updates.begin().key(); // NOTE maybe some actions have more priority than another
            auto apps = updates.values(action);
            for (auto app: apps) {
                QList<QDomElement> xml;
                OutgoingUpdateCB callback;
                std::tie(xml, callback) = app->takeOutgoingUpdate();
                updateXml += xml;
                if (callback) {
                    acceptApps.append(AckHndl{app, callback});
                }
            }
            sendJingle(action, updateXml, [this, acceptApps](){
                for (const auto &h: acceptApps) {
                    auto app = std::get<0>(h);
                    auto callback = std::get<1>(h);
                    if (app) {
                        callback();
                    }
                }
                planStep();
            });
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

    void addAndInitContent(Origin creator, Application *content)
    {
        contentList.insert(ContentKey{content->contentName(), creator}, content);
        if (state != State::Created && content->evaluateOutgoingUpdate() != Action::NoAction) {
            signalingContent.insert(content);
        }
        QObject::connect(content, &Application::updated, q, [this, content](){
            signalingContent.insert(content);
            planStep();
        });
        QObject::connect(content, &Application::destroyed, q, [this, content](){
            signalingContent.remove(content);
            for (auto it = contentList.begin(); it != contentList.end(); ++it) { // optimize for large lists?
                if (it.value() == content) {
                    contentList.erase(it);
                    break;
                }
            }
        });
    }

    enum AddContentError {
        Ok,
        Unparsed,
        Unexpected,
        Unsupported
    };

    std::tuple<AddContentError, Reason::Condition, Application*> parseContentAdd(const QDomElement &ce)
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


    typedef std::tuple<AddContentError, Reason::Condition, QList<Application*>, QList<QDomElement>> ParseContentListResult;

    ParseContentListResult parseContentAddList(const QDomElement &jingleEl)
    {
        QMap<QString,Application *> addSet;
        QMap<QString,std::pair<QDomElement,Reason::Condition>> rejectSet;

        QString contentTag(QStringLiteral("content"));
        for(QDomElement ce = jingleEl.firstChildElement(contentTag);
            !ce.isNull(); ce = ce.nextSiblingElement(contentTag))
        {

            Private::AddContentError err;
            Reason::Condition cond;
            Application *app;

            std::tie(err, cond, app) = parseContentAdd(ce);
            if (err == Private::AddContentError::Unparsed) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                qDeleteAll(addSet);
                return ParseContentListResult(Unparsed, cond, QList<Application*>(), QList<QDomElement>());
            }

            auto contentName = app->contentName();
            auto it = addSet.find(contentName);
            if (err != Private::AddContentError::Ok) {
                // can't continue as well
                if (app) { // we are going to reject it completely so delete
                    delete app;
                }

                if (it == addSet.end()) {
                    rejectSet.insert(contentName, std::make_pair(ce, cond));
                }
                continue;
            }

            rejectSet.remove(contentName);
            if (it == addSet.end() || (*it)->wantBetterTransport(app->transport())) { // probably not wantBetterTransport but wantBetterApplication
                if (it == addSet.end()) {
                    addSet.insert(contentName, app);
                } else {
                    delete *it; // unpreferred app
                    *it = app;
                }
            }
        }

        if (rejectSet.size()) {
            QList<QDomElement> rejectList;
            for (auto const &i: rejectSet) {
                rejectList.append(i.first);
            }
            return ParseContentListResult(Unsupported, rejectSet.first().second, addSet.values(), rejectList);
        }

        return ParseContentListResult(Ok, Reason::Success, addSet.values(), QList<QDomElement>());
    }

    std::tuple<AddContentError, Reason::Condition, Application*> parseContentAccept(const QDomElement &ce)
    {
        QDomElement descriptionEl = ce.firstChildElement(QLatin1String("description"));
        QDomElement transportEl = ce.firstChildElement(QLatin1String("transport"));
        QString descriptionNS = descriptionEl.namespaceURI();
        QString transportNS = transportEl.namespaceURI();
        typedef std::tuple<AddContentError, Reason::Condition, Application*> result;

        ContentBase c(ce);
        if (!c.isValid() || role != c.creator || descriptionEl.isNull() || transportEl.isNull() ||
                descriptionNS.isEmpty() || transportNS.isEmpty())
        {
            return result{Unparsed, Reason::NoReason, nullptr};
        }

        auto app = q->content(c.name, role);
        if (!(app && app->state() == State::Pending)) { // reaccept is possible
            return result{AddContentError::Unexpected, Reason::NoReason, app};
        }

        if (app->pad()->ns() != descriptionNS || app->transport()->pad()->ns() != transportNS) {
            // well it's more than unexpected. let's send unparsed
            return result{AddContentError::Unparsed, Reason::NoReason, app};
        }

        if (!app->transport()->update(transportEl) || !app->accept(descriptionEl)) {
            // clearly unparsed. otherwise the app will generate failure event with a Reason.
            return result{AddContentError::Unparsed, Reason::NoReason, app};
        }

        if (app->state() != State::Accepted) { // parsed but was not accepted. so it's somehow incompatible
            return result{AddContentError::Unsupported, Reason::IncompatibleParameters, app};
        }

        return result{AddContentError::Ok, Reason::Success, app};
    }

    std::tuple<bool,QList<Application*>> parseContentAcceptList(const QDomElement &jingleEl)
    {
        QMap<QString,Application *> acceptSet;
        QMap<QString,std::pair<QDomElement,Reason::Condition>> rejectSet;

        QString contentTag(QStringLiteral("content"));
        for(QDomElement ce = jingleEl.firstChildElement(contentTag);
            !ce.isNull(); ce = ce.nextSiblingElement(contentTag))
        {

            Private::AddContentError err;
            Reason::Condition cond;
            Application *app;

            std::tie(err, cond, app) = parseContentAccept(ce);
            if (err == Private::AddContentError::Unparsed || err == Private::AddContentError::Unexpected) {
                for (auto &a: acceptSet) {
                    a->setState(State::Pending); // reset state to pending for already passed validation before passing error back
                }
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, err == Private::AddContentError::Unexpected?
                                                    XMPP::Stanza::Error::UnexpectedRequest : XMPP::Stanza::Error::BadRequest);
                if (err == Private::AddContentError::Unexpected) {
                    ErrorUtil::fill(jingleEl.ownerDocument(), lastError, ErrorUtil::OutOfOrder);
                }
                return std::tuple<bool,QList<Application*>>(false, QList<Application*>());
            }

            auto contentName = app->contentName();
            auto it = acceptSet.find(contentName);
            auto rit = rejectSet.find(contentName);
            if (it != acceptSet.end() || rit != rejectSet.end()) {
                // duplicates are not allowed in accept request
                for (auto &a: acceptSet) {
                    a->setState(State::Pending); // reset state to pending for already passed validation before passing error back
                }
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return std::tuple<bool,QList<Application*>>(false, QList<Application*>());
            }

            if (err != Private::AddContentError::Ok) {
                app->setState(State::Finished); // we can't keep working with this content for whatever reason. if "accept" failed there is no fallback
                rejectSet.insert(contentName, std::make_pair(ce, cond)); // NOTE, probably instead of ce we have to generate original description
                continue;
            }
            acceptSet.insert(contentName, app);
        }

        if (rejectSet.size()) {
            QTimer::singleShot(0, q, [this, rejectSet]() mutable {
                auto cond = rejectSet.first().second;
                QList<QDomElement> rejects;
                for (auto const &i: rejectSet) {
                    rejects.append(i.first);
                }
                rejects += Reason(cond).toXml(manager->client()->doc());
                outgoingUpdates.insert(
                            Action::ContentRemove,
                            OutgoingUpdate{
                                rejects,
                                [this, rejects]() {
                                    for (auto &r: rejects) {
                                        ContentBase c(r);
                                        delete contentList.take(ContentKey{c.name, role});
                                    }
                                    if (contentList.isEmpty()) {
                                        // the other party has to generate session-terminate but we do not care already
                                        setSessionFinished();
                                    }
                                }
                            });
            });
        }
        return std::tuple<bool,QList<Application*>>(true, acceptSet.values());
    }


    bool handleIncomingContentAdd(const QDomElement &jingleEl)
    {
        Private::AddContentError err;
        Reason::Condition cond;
        QList<Application *> apps;
        QList<QDomElement> rejects;

        std::tie(err, cond, apps, rejects) = parseContentAddList(jingleEl);
        switch (err) {
        case Private::AddContentError::Unparsed:
        case Private::AddContentError::Unexpected:
            lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
            if (err == Private::AddContentError::Unexpected) {
                ErrorUtil::fill(jingleEl.ownerDocument(), lastError, ErrorUtil::OutOfOrder);
            }
            return false;
        case Private::AddContentError::Unsupported:
            rejects += Reason(cond).toXml(manager->client()->doc());
            outgoingUpdates.insert(Action::ContentReject, OutgoingUpdate{rejects, OutgoingUpdateCB()});
            break;
        case Private::AddContentError::Ok:
            break;
        }

        if (apps.size()) {
            Origin remoteRole = negateOrigin(role);
            for (auto app: apps) {
                addAndInitContent(remoteRole, app); // TODO check conflicts
            }
            QTimer::singleShot(0, q, [this](){ emit q->newContentReceived(); });
        }
        planStep();

        return true;
    }

    bool handleIncomingContentRemove(const QDomElement &jingleEl)
    {
        QSet<Application*> toRemove;
        QString contentTag(QStringLiteral("content"));
        for(QDomElement ce = jingleEl.firstChildElement(contentTag);
            !ce.isNull(); ce = ce.nextSiblingElement(contentTag))
        {
            ContentBase cb(ce);
            if (!cb.isValid()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }
            Application *app = contentList.value(ContentKey{cb.name, cb.creator});
            if (app) {
                toRemove.insert(app);
            }
        }

        for (auto app: toRemove) {
            contentList.remove(ContentKey{app->contentName(), app->creator()});
            delete app;
        }

        if (contentList.isEmpty()) {
            auto reasonEl = jingleEl.firstChildElement(QString::fromLatin1("reason"));
            terminateReason = reasonEl.isNull()? Reason(Reason::Success) : Reason(reasonEl);
        }

        planStep();
        return true;
    }

    bool handleIncomingSessionTerminate(const QDomElement &jingleEl)
    {
        terminateReason = Reason(jingleEl.firstChildElement(QString::fromLatin1("reason")));
        setSessionFinished();
        return true;
    }

    bool handleIncomingSessionAccept(const QDomElement &jingleEl)
    {
        bool parsed;
        QList<Application *> apps;

        std::tie(parsed, apps) = parseContentAcceptList(jingleEl);
        if (!parsed) {
            lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
            return false;
        }

        state = State::Connecting;
        if (apps.size()) {
            for (auto app: apps) {
                app->start();
            }
        }
        QTimer::singleShot(0, q, [this](){ emit q->activated(); });
        planStep();

        return true;
    }

    bool handleIncomingContentAccept(const QDomElement &jingleEl)
    {
        bool parsed;
        QList<Application *> apps;

        std::tie(parsed, apps) = parseContentAcceptList(jingleEl); // marks valid apps as accepted
        if (!parsed) {
            lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
            return false;
        }

        if (apps.size() && state >= State::Active) {
            for (auto app: apps) {
                app->start(); // start accepted app. connection establishing and data transfer are inside
            }
        }
        planStep();

        return true;
    }

    bool handleIncomingTransportReplace(const QDomElement &jingleEl)
    {
        QList<std::tuple<Application*,QSharedPointer<Transport>,QDomElement>> passed;
        QList<QDomElement> toReject;
        QString contentTag(QStringLiteral("content"));
        for(QDomElement ce = jingleEl.firstChildElement(contentTag);
            !ce.isNull(); ce = ce.nextSiblingElement(contentTag))
        {
            ContentBase cb(ce);
            auto transportEl = ce.firstChildElement(QString::fromLatin1("transport"));
            QString transportNS = transportEl.namespaceURI();
            if (!cb.isValid() || transportEl.isNull() || transportNS.isEmpty()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }
            Application *app = contentList.value(ContentKey{cb.name, cb.creator});
            if (!app) {
                toReject.append(ce);
                continue;
            }

            auto trPad = q->transportPadFactory(transportNS);
            if (!trPad) {
                toReject.append(ce);
                continue;
            }

            auto transport = trPad->manager()->newTransport(trPad, transportEl);
            if (!transport) {
                toReject.append(ce);
                continue;
            }

            auto tfo = app->transportReplaceOrigin();
            // if transport recovery is in progress and I as an inititiator started it then forbid remote transport-replace
            if (tfo != Origin::None && tfo != q->peerRole() && role == Origin::Initiator) {
                lastError = ErrorUtil::makeTieBreak(*manager->client()->doc());
                return false;
            }

            passed.append(std::make_tuple(app, transport, ce));
        }

        for (auto &v: passed) {
            Application *app;
            QSharedPointer<Transport> transport;
            QDomElement ce;
            std::tie(app,transport,ce) = v;
            if (!app->incomingTransportReplace(transport)) { // app should generate transport accept eventually. content-accept will work too if the content wasn't accepted yet
                toReject.append(ce);
            }
        }

        if (toReject.size()) {
            outgoingUpdates.insert(Action::TransportReject, OutgoingUpdate{toReject,OutgoingUpdateCB()});
        }

        planStep();
        return true;
    }

    bool handleIncomingTransportAccept(const QDomElement &jingleEl)
    {
        QString contentTag(QStringLiteral("content"));
        for(QDomElement ce = jingleEl.firstChildElement(contentTag);
            !ce.isNull(); ce = ce.nextSiblingElement(contentTag))
        {
            ContentBase cb(ce);
            auto transportEl = ce.firstChildElement(QString::fromLatin1("transport"));
            QString transportNS = transportEl.namespaceURI();
            if (!cb.isValid() || transportEl.isNull() || transportNS.isEmpty()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }
            Application *app = contentList.value(ContentKey{cb.name, cb.creator});
            if (!app || !app->transport() || app->transport()->pad()->ns() != transportNS || !app->incomingTransportAccept(transportEl)) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }
        }

        planStep();
        return true;
    }

    bool handleIncomingTransportInfo(const QDomElement &jingleEl)
    {
        QString contentTag(QStringLiteral("content"));
        QList<QPair<QSharedPointer<Transport>,QDomElement>> updates;
        for(QDomElement ce = jingleEl.firstChildElement(contentTag);
            !ce.isNull(); ce = ce.nextSiblingElement(contentTag))
        {
            Application *app;
            ContentBase cb(ce);
            if (!cb.isValid() || !(app = q->content(cb.name, cb.creator)) || app->state() >= State::Finishing || !app->transport()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }
            auto tel = ce.firstChildElement(QStringLiteral("transport"));
            if (tel.isNull() || tel.namespaceURI() != app->transport()->pad()->ns()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }
            updates.append(qMakePair(app->transport(), tel));
        }

        for (auto &u: updates) {
            if (!u.first->update(u.second)) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false; // yes this may lead to half updated content, but what choice do we have?
            }
        }

        return true;
    }
};

Session::Session(Manager *manager, const Jid &peer, Origin role) :
    d(new Private)
{
    d->q = this;
    d->role = role;
    d->manager = manager;
    d->otherParty = peer;
    d->stepTimer.setSingleShot(true);
    d->stepTimer.setInterval(0);
    connect(&d->stepTimer, &QTimer::timeout, this, [this](){ d->doStep();});

}

Session::~Session()
{
    qDeleteAll(d->contentList);
    qDebug("session %s destroyed", qPrintable(d->sid));
}

Manager *Session::manager() const
{
    return d->manager;
}

State Session::state() const
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

QString Session::sid() const
{
    return d->sid;
}

Origin Session::role() const
{
    return d->role;
}

Origin Session::peerRole() const
{
    return negateOrigin(d->role);
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
    return d->contentList.value(ContentKey{contentName, creator});
}

void Session::addContent(Application *content)
{
    Q_ASSERT(d->state < State::Finishing);
    d->addAndInitContent(d->role, content);
    if (d->state >= State::PrepareLocalOffer) {
        // If we add content to already initiated session then we are gonna
        // send it immediatelly. So start prepare
        content->prepare();
    }
}

const QMap<ContentKey, Application *> &Session::contentList() const
{
    return d->contentList;
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
    if (d->applicationPads.size()) {
        return d->applicationPads.constBegin().key();
    }
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

void Session::accept()
{
    // So we presented a user incoming session in UI, the user modified it somehow and finally accepted.
    if (d->role == Origin::Responder && d->state == State::Created) {
        d->state = State::PrepareLocalOffer;
        for (auto &c: d->contentList) {
            c->prepare();
        }
        d->planStep();
    }
}

void Session::initiate()
{
    emit initiated();
    if (d->role == Origin::Initiator && d->state == State::Created) {
        d->state = State::PrepareLocalOffer;
        for (auto &c: d->contentList) {
            c->prepare();
        }
        d->planStep();
    }
}

void Session::terminate(Reason::Condition cond, const QString &comment)
{
    if (d->role == Origin::Initiator && d->state == State::PrepareLocalOffer) {
        d->setSessionFinished();
        return;
    }
    d->state = State::Finishing;
    d->terminateReason = Reason(cond, comment);
    d->planStep();
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

    Private::AddContentError err;
    Reason::Condition cond;
    QList<Application *> apps;
    QList<QDomElement> rejects;

    std::tie(err, cond, apps, rejects) = d->parseContentAddList(jingleEl);
    switch (err) {
    case Private::AddContentError::Unparsed:
    case Private::AddContentError::Unexpected:
        return false;
    case Private::AddContentError::Unsupported:
        d->terminateReason = Reason(cond);
        d->planStep();
        return true;
    case Private::AddContentError::Ok:
        for (auto app: apps) {
            d->addAndInitContent(Origin::Initiator, app);
        }
        d->planStep();
        return true;
    }

    return false;
}

bool Session::updateFromXml(Action action, const QDomElement &jingleEl)
{
    if (d->state == State::Finished) {
        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::UnexpectedRequest);
        ErrorUtil::fill(jingleEl.ownerDocument(), d->lastError, ErrorUtil::OutOfOrder);
        return false;
    }

    switch (action) {
    case Action::ContentAccept:
        return d->handleIncomingContentAccept(jingleEl);
    case Action::ContentAdd:
        return d->handleIncomingContentAdd(jingleEl);
    case Action::ContentModify:
        break;
    case Action::ContentReject:
        break;
    case Action::ContentRemove:
        return d->handleIncomingContentRemove(jingleEl);
    case Action::DescriptionInfo:
        break;
    case Action::SecurityInfo:
        break;
    case Action::SessionAccept:
        return d->handleIncomingSessionAccept(jingleEl);
    case Action::SessionInfo:
        break;
    case Action::SessionInitiate: // impossible case. but let compiler be happy
        break;
    case Action::SessionTerminate:
        return d->handleIncomingSessionTerminate(jingleEl);
    case Action::TransportAccept:
        return d->handleIncomingTransportAccept(jingleEl);
    case Action::TransportInfo:
        return d->handleIncomingTransportInfo(jingleEl);
    case Action::TransportReject:
        break;
    case Action::TransportReplace:
        return d->handleIncomingTransportReplace(jingleEl);
    case Action::NoAction:
    default:
        break;
    }

    d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::FeatureNotImplemented);
    return false;
}


//----------------------------------------------------------------------------
// SessionManagerPad - handle event related to a type of app/transport but not specific instance
//----------------------------------------------------------------------------
QDomElement SessionManagerPad::takeOutgoingSessionInfoUpdate()
{
    return QDomElement();
}

QDomDocument *SessionManagerPad::doc() const
{
    return session()->manager()->client()->doc();
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

    void setupSession(Session *s)
    {
        QObject::connect(s, &Session::terminated, manager, [this, s](){
            sessions.remove(qMakePair(s->peer(), s->sid()));
        });
    }
};

Manager::Manager(Client *client) :
    QObject(client),
    d(new Private())
{
    d->client = client;
    d->manager = this;
    d->pushTask.reset(new JTPush(client->rootTask()));
}

Manager::~Manager()
{
    for (auto &m: d->transportManagers) {
        m->setJingleManager(nullptr);
    }
    for (auto &m: d->applicationManagers) {
        m->setJingleManager(nullptr);
    }
}

Client *Manager::client() const
{
    return d->client;
}

void Manager::addExternalManager(const QString &ns)
{
    d->pushTask->addExternalManager(ns);
}

void Manager::forgetExternalSession(const QString &sid)
{
    d->pushTask->forgetExternalSession(sid);
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

void Manager::detachSession(Session *s)
{
    s->disconnect(this);
    d->sessions.remove(qMakePair(s->peer(), s->sid()));
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
    auto s = new Session(this, from, Origin::Responder);
    if (s->incomingInitiate(jingle, jingleEl)) { // if parsed well
        d->sessions.insert(key, s);
        d->setupSession(s);
        // emit incomingSession makes sense when there are no unsolved conflicts in content descriptions / transports
        //QTimer::singleShot(0,[s, this](){ emit incomingSession(s); });
        QMetaObject::invokeMethod(this, "incomingSession", Qt::QueuedConnection, Q_ARG(Session*, s));
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
    auto s = new Session(this, j);
    d->setupSession(s);
    return s;
}

QString Manager::registerSession(Session *session)
{
    QString id;
    auto peer = session->peer();
    do {
        id = QString("%1").arg(quint32(qrand()), 6, 32, QChar('0'));
    } while (d->sessions.contains(qMakePair(peer,id)));
    d->sessions.insert(qMakePair(peer, id), session);
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

bool Connection::hasPendingDatagrams() const
{
    return false;
}

NetworkDatagram Connection::receiveDatagram(qint64 maxSize)
{
    Q_UNUSED(maxSize)
    return NetworkDatagram();
}

size_t Connection::blockSize() const
{
    return 0; // means "block" is not applicable for this kind of connection
}

const char* ErrorUtil::names[ErrorUtil::Last] = {"out-of-order","tie-break", "unknown-session", "unsupported-info"};

Stanza::Error ErrorUtil::make(QDomDocument &doc, int jingleCond, int type, int condition, const QString &text)
{
    auto el = doc.createElementNS(ERROR_NS, QString::fromLatin1(names[jingleCond - 1]));
    return Stanza::Error(type, condition, text, el);
}

void ErrorUtil::fill(QDomDocument doc, Stanza::Error &error, int jingleCond)
{
    error.appSpec = doc.createElementNS(ERROR_NS, QString::fromLatin1(names[jingleCond - 1]));
}

int ErrorUtil::jingleCondition(const Stanza::Error &error)
{
    if (error.appSpec.namespaceURI() != ERROR_NS) {
        return UnknownError;
    }
    QString tagName = error.appSpec.tagName();
    for (int i = 0; i < int(sizeof(names) / sizeof(names[0])); ++i) {
        if (tagName == names[i]) {
            return i + 1;
        }
    }
    return UnknownError;
}

Stanza::Error ErrorUtil::makeTieBreak(QDomDocument &doc)
{
    return make(doc, TieBreak, XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::Conflict);
}

Stanza::Error ErrorUtil::makeOutOfOrder(QDomDocument &doc)
{
    return make(doc, OutOfOrder, XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::UnexpectedRequest);
}


} // namespace Jingle
} // namespace XMPP

#include "jingle.moc"
