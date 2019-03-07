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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
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
};

Jingle::Jingle()
{

}

Jingle::Jingle(Jingle::Action action, const QString &sid) :
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
    if (!d || d->sid.isEmpty() || d->action == NoAction) {
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

Jingle::Action Jingle::action() const
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
class ApplicationManager::Private
{
public:
    Client *client;
};

ApplicationManager::ApplicationManager(Client *client) :
    d(new Private)
{
    d->client = client;
}

ApplicationManager::~ApplicationManager()
{

}

Client *ApplicationManager::client() const
{
    return d->client;
}

//----------------------------------------------------------------------------
// TransportManager
//----------------------------------------------------------------------------
TransportManager::TransportManager(Manager *jingleManager) :
    QObject(jingleManager)
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
        if (jingle.action() == Jingle::SessionInitiate) {
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

    Stanza::Error error_;
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
    Manager *manager;
    QTimer stepTimer;
    Session::State state = Session::Starting;
    Session::Role  role  = Session::Role::Initiator; // my role in the session
    XMPP::Stanza::Error lastError;
    QMap<QString,SessionManagerPad*> managerPads;
    QMap<QString,Application*> myContent;     // content::creator=(role == Session::Role::Initiator?initiator:responder)
    QMap<QString,Application*> remoteContent; // content::creator=(role == Session::Role::Responder?initiator:responder)
    QString sid;
    Jid origFrom; // "from" attr of IQ.
    Jid otherParty; // either "from" or initiator/responder. it's where to send all requests.
    bool waitingAck = false;

    void sendJingle(Jingle::Action action, QList<QDomElement> update)
    {
        QDomDocument &doc = *manager->client()->doc();
        Jingle jingle(action, sid);
        if (action == Jingle::Action::SessionInitiate) {
            jingle.setInitiator(manager->client()->jid());
        }
        if (action == Jingle::Action::SessionAccept) {
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
        if (!stepTimer.isActive()) {
            stepTimer.start();
        }
    }

    void doStep() {
        if (waitingAck) { // we will return here when ack is received
            return;
        }
        QList<QDomElement> updateXml;
        for (auto mp : managerPads) {
            QDomElement el = mp->takeOutgoingSessionInfoUpdate();
            if (!el.isNull()) {
                updateXml.append(el);
                // we can send session-info for just one application. so stop processing
                sendJingle(Jingle::Action::SecurityInfo, updateXml);
                return;
            }
        }

        bool needCheckAcceptance = (role == Session::Role::Responder && state != Session::State::Active);
        bool needAccept = needCheckAcceptance;
        QMultiMap<Jingle::Action, Application*> updates;

        for (auto app : myContent) {
            Jingle::Action updateType = app->outgoingUpdateType();
            if (updateType != Jingle::Action::NoAction) {
                updates.insert(updateType, app);
                needAccept = false;
            } else if (needCheckAcceptance && !app->isReadyForSessionAccept()) {
                needAccept = false;
            }
        }

        // the same for remote. where is boost::join in Qt?
        for (auto app : remoteContent) {
            Jingle::Action updateType = app->outgoingUpdateType();
            if (updateType != Jingle::Action::NoAction) {
                updates.insert(updateType, app);
                needAccept = false;
            } else if (needCheckAcceptance && !app->isReadyForSessionAccept()) {
                needAccept = false;
            }
        }

        if (updates.size()) {
            Jingle::Action action = updates.begin().key();
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
            sendJingle(Jingle::Action::SessionAccept, updateXml);
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
};

Session::Session(Manager *manager) :
    d(new Private)
{
    d->manager = manager;
    d->stepTimer.setSingleShot(true);
    d->stepTimer.setInterval(0);
    connect(&d->stepTimer, &QTimer::timeout, this, [this](){ d->doStep();});

}

Session::~Session()
{

}

Session::State Session::state() const
{
    return d->state;
}

Jid Session::peer() const
{
    return d->otherParty;
}

XMPP::Stanza::Error Session::lastError() const
{
    return d->lastError;
}

QStringList Session::allManagerPads() const
{
    return d->managerPads.keys();
}

SessionManagerPad *Session::pad(const QString &ns)
{
    return d->managerPads.value(ns);
}

QString Session::preferredApplication() const
{
    // TODO some heuristics to detect preferred application
    return QString();
}

QStringList Session::allApplicationTypes() const
{
    QSet<QString> ret;
    for (auto const &k: d->managerPads.keys()) {
        if (d->manager->isRegisteredApplication(k))
            ret.insert(k);
    }
    return ret.toList();
}

void Session::reject()
{
    // TODO
}

void Session::deleteUnusedPads()
{
    // TODO
}

bool Session::addContent(const QDomElement &ce)
{
    QDomElement descriptionEl = ce.firstChildElement(QLatin1String("description"));
    QDomElement transportEl = ce.firstChildElement(QLatin1String("transport"));
    QString descriptionNS = descriptionEl.namespaceURI();
    QString transportNS = transportEl.namespaceURI();

    ContentBase c(ce);
    if (!c.isValid() || descriptionEl.isNull() || transportEl.isNull() || descriptionNS.isEmpty() || transportNS.isEmpty()) {
        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
        return false;
    }

    SessionManagerPad *pad = d->managerPads.value(descriptionNS);
    if (!pad) {
        pad = d->manager->applicationPad(this, descriptionNS);
        d->managerPads.insert(descriptionNS, pad);
    }
    auto app = d->manager->startApplication(pad, c.creator, c.senders);
    if (app->setDescription(descriptionEl)) {
        // same for transport
        pad = d->managerPads.value(transportNS);
        if (!pad) {
            auto *pad = d->manager->transportPad(transportNS);
            d->managerPads.insert(transportNS, pad);
        }
        auto transport = d->manager->initTransport(pad, d->origFrom, transportEl);
        if (transport) {
            app->setTransport(transport);
            return true;
        }
        delete app;
    }

    d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
    deleteUnusedPads();
    return false;
}

bool Session::incomingInitiate(const Jid &from, const Jingle &jingle, const QDomElement &jingleEl)
{
    d->sid = jingle.sid();
    d->origFrom = from;
    d->otherParty = jingle.initiator().isValid()? jingle.initiator() : from;
    //auto key = qMakePair(from, jingle.sid());

    QString contentTag(QStringLiteral("content"));
    for(QDomElement ce = jingleEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        if (!addContent(ce)) { // not parsed
            return false;
        }
    }

    d->planStep();
    return true;
    //QDomElement securityEl = content.firstChildElement(QLatin1String("security"));
/*
    if (jingle->action() == Jingle::Action::SessionInitiate   ||
            jingle->action() == Jingle::Action::SessionAccept ||
            jingle->action() == Jingle::Action::ContentAdd    ||
            jingle->action() == Jingle::Action::ContentAccept ||
            jingle->action() == Jingle::Action::ContentReject ||
            jingle->action() == Jingle::Action::DescriptionInfo)
    {
        description = manager->descriptionFromXml(descriptionEl);
        if (description.isNull()) {
            return;
        }
    } // else description is unexpected. log it?

    if (jingle->action() == Jingle::Action::SessionInitiate   ||
            jingle->action() == Jingle::Action::SessionAccept ||
            jingle->action() == Jingle::Action::ContentAdd    ||
            jingle->action() == Jingle::Action::ContentAccept ||
            jingle->action() == Jingle::Action::ContentReject)
    {
        // content-reject posses empty transport
        auto transport = manager->transportFromXml(transportEl);
    }
    */
    /*
    if (!securityEl.isNull()) {
        security = client->jingleManager()->securityFromXml(securityEl);
        // if security == 0 then then its unsupported? just ignore it atm
        // according to xtls draft responder may omit security when unsupported.
    }
    */

    // TODO description
    // TODO transports
    // TODO security

    //return client()->jingleManager()->incomingIQ(el);
}

bool Session::updateFromXml(Jingle::Action action, const QDomElement &jingleEl)
{
    if (action == Jingle::SessionInfo) {

    }

    QString contentTag(QStringLiteral("content"));
    for(QDomElement ce = jingleEl.firstChildElement(contentTag);
        !ce.isNull(); ce = ce.nextSiblingElement(contentTag)) {
        if (!addContent(ce)) { // not parsed
            return false;
        }
    }
    return false;
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

Application* Manager::startApplication(SessionManagerPad *pad, Origin creator, Origin senders)
{
    auto appManager = d->applicationManagers.value(pad->ns());
    if (!appManager) {
        return NULL;
    }
    return appManager->startApplication(pad, creator, senders);
}

SessionManagerPad *Manager::applicationPad(Session *session, const QString &ns)
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
}

void Manager::unregisterTransport(const QString &ns)
{
    auto trManager = d->transportManagers.value(ns);
    if (trManager) {
        trManager->closeAll();
        d->transportManagers.remove(ns);
    }
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

void Manager::setRemoteJidChecked(std::function<bool(const Jid &)> checker)
{
    d->remoteJidCecker = checker;
}

QSharedPointer<Transport> Manager::initTransport(SessionManagerPad *pad,
                                                 const Jid &jid, const QDomElement &el)
{
    auto transportManager = d->transportManagers.value(el.namespaceURI());
    if (!transportManager) {
        return QSharedPointer<Transport>();
    }
    return transportManager->sessionInitiate(pad, jid, el);
}

SessionManagerPad *Manager::transportPad(const QString &ns)
{
    auto transportManager = d->transportManagers.value(ns);
    if (!transportManager) {
        return NULL;
    }
    return transportManager->pad();
}

Session* Manager::incomingSessionInitiate(const Jid &from, const Jingle &jingle, const QDomElement &jingleEl)
{
    if (d->maxSessions > 0 && d->sessions.size() == d->maxSessions) {
        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Wait, XMPP::Stanza::Error::ResourceConstraint);
        return NULL;
    }
    auto key = qMakePair(from, jingle.sid());
    auto s = new Session(this);
    if (s->incomingInitiate(from, jingle, jingleEl)) { // if parsed well
        d->sessions.insert(key, s);
        // emit incomingSession makes sense when there is no unsolved conflicts in content descriptions / transports
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
    Q_UNUSED(j); // TODO
    auto s = new Session(this);
    return s;
}


} // namespace Jingle
} // namespace XMPP

#include "jingle.moc"
