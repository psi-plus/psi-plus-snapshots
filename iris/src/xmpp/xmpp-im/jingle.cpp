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

#include "jingle-application.h"
#include "jingle-session.h"
#include "xmpp-im/xmpp_hash.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_stream.h"
#include "xmpp_task.h"
#include "xmpp_xmlcommon.h"

#include <QDateTime>
#include <QDebug>
#include <QDomElement>
#include <QMap>
#include <QPointer>
#include <QTimer>
#include <functional>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <QCoreApplication>

namespace XMPP { namespace Jingle {
    const QString NS(QStringLiteral("urn:xmpp:jingle:1"));
    const QString ERROR_NS(QStringLiteral("urn:xmpp:jingle:errors:1"));

    //----------------------------------------------------------------------------
    // Jingle
    //----------------------------------------------------------------------------
    static const struct {
        const char *text;
        Action      action;
    } jingleActions[]
        = { { "content-accept", Action::ContentAccept },       { "content-add", Action::ContentAdd },
            { "content-modify", Action::ContentModify },       { "content-reject", Action::ContentReject },
            { "content-remove", Action::ContentRemove },       { "description-info", Action::DescriptionInfo },
            { "security-info", Action::SecurityInfo },         { "session-accept", Action::SessionAccept },
            { "session-info", Action::SessionInfo },           { "session-initiate", Action::SessionInitiate },
            { "session-terminate", Action::SessionTerminate }, { "transport-accept", Action::TransportAccept },
            { "transport-info", Action::TransportInfo },       { "transport-reject", Action::TransportReject },
            { "transport-replace", Action::TransportReplace } };

    Origin negateOrigin(Origin o)
    {
        switch (o) {
        case Origin::None:
            return Origin::Both;
        case Origin::Both:
            return Origin::None;
        case Origin::Initiator:
            return Origin::Responder;
        case Origin::Responder:
            return Origin::Initiator;
        }
        return Origin::None;
    }

    class Jingle::Private : public QSharedData {
    public:
        Action  action;
        QString sid;
        Jid     initiator;
        Jid     responder;
    };

    Jingle::Jingle() { }

    Jingle::Jingle(Action action, const QString &sid) : d(new Private)
    {
        d->action = action;
        d->sid    = sid;
    }

    Jingle::Jingle(const QDomElement &e)
    {
        QString actionStr = e.attribute(QLatin1String("action"));
        Action  action    = Action::NoAction;
        QString sid       = e.attribute(QLatin1String("sid"));
        Jid     initiator;
        Jid     responder;

        for (unsigned int i = 0; i < sizeof(jingleActions) / sizeof(jingleActions[0]); i++) {
            if (actionStr == jingleActions[i].text) {
                action = jingleActions[i].action;
                break;
            }
        }
        if (action == Action::NoAction || sid.isEmpty()) {
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

        d            = new Private;
        d->action    = action;
        d->sid       = sid;
        d->responder = responder;
    }

    Jingle::Jingle(const Jingle &other) : d(other.d) { }

    Jingle::~Jingle() { }

    Jingle::Private *Jingle::ensureD()
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

        if (!d->initiator.isNull())
            query.setAttribute(QLatin1String("initiator"), d->initiator.full());
        if (!d->responder.isNull())
            query.setAttribute(QLatin1String("responder"), d->responder.full());
        query.setAttribute(QLatin1String("sid"), d->sid);

        return query;
    }

    Action Jingle::action() const { return d->action; }

    const QString &Jingle::sid() const { return d->sid; }

    const Jid &Jingle::initiator() const { return d->initiator; }

    void Jingle::setInitiator(const Jid &jid) { d->initiator = jid; }

    const Jid &Jingle::responder() const { return d->responder; }

    void Jingle::setResponder(const Jid &jid) { d->responder = jid; }

    //----------------------------------------------------------------------------
    // Reason
    //----------------------------------------------------------------------------
    using ReasonMap = QMap<QString, Reason::Condition>;

    Q_GLOBAL_STATIC_WITH_ARGS(ReasonMap, reasonConditions,
                              ({
                                  { QLatin1String("alternative-session"), Reason::AlternativeSession },
                                  { QLatin1String("busy"), Reason::Busy },
                                  { QLatin1String("cancel"), Reason::Cancel },
                                  { QLatin1String("connectivity-error"), Reason::ConnectivityError },
                                  { QLatin1String("decline"), Reason::Decline },
                                  { QLatin1String("expired"), Reason::Expired },
                                  { QLatin1String("failed-application"), Reason::FailedApplication },
                                  { QLatin1String("failed-transport"), Reason::FailedTransport },
                                  { QLatin1String("general-error"), Reason::GeneralError },
                                  { QLatin1String("gone"), Reason::Gone },
                                  { QLatin1String("incompatible-parameters"), Reason::IncompatibleParameters },
                                  { QLatin1String("media-error"), Reason::MediaError },
                                  { QLatin1String("security-error"), Reason::SecurityError },
                                  { QLatin1String("success"), Reason::Success },
                                  { QLatin1String("timeout"), Reason::Timeout },
                                  { QLatin1String("unsupported-applications"), Reason::UnsupportedApplications },
                                  { QLatin1String("unsupported-transports"), Reason::UnsupportedTransports },
                              }))

    class Reason::Private : public QSharedData {
    public:
        Reason::Condition cond;
        QString           text;
    };

    Reason::Reason() { }

    Reason::~Reason() { }

    Reason::Reason(Reason::Condition cond, const QString &text) : d(new Private)
    {
        d->cond = cond;
        d->text = text;
    }

    Reason::Reason(const QDomElement &e)
    {
        if (e.tagName() != QLatin1String("reason"))
            return;

        Condition condition = NoReason;
        QString   text;
        QString   rns = e.namespaceURI();

        for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
            if (c.tagName() == QLatin1String("text")) {
                text = c.text();
            } else if (c.namespaceURI() != rns) {
                // TODO add here all the extensions to reason.
            } else {
                condition = reasonConditions->value(c.tagName());
            }
        }

        if (condition != NoReason) {
            d       = new Private;
            d->cond = condition;
            d->text = text;
        }
    }

    Reason::Reason(const Reason &other) : d(other.d) { }

    Reason &Reason::operator=(const Reason &other)
    {
        d = other.d;
        return *this;
    }

    Reason::Condition Reason::condition() const
    {
        if (d)
            return d->cond;
        return NoReason;
    }

    void Reason::setCondition(Condition cond) { ensureD()->cond = cond; }

    QString Reason::text() const
    {
        if (d)
            return d->text;
        return QString();
    }

    void Reason::setText(const QString &text) { ensureD()->text = text; }

    QDomElement Reason::toXml(QDomDocument *doc) const
    {
        if (d && d->cond != NoReason) {
            for (auto r = reasonConditions->cbegin(); r != reasonConditions->cend(); ++r) {
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

    Reason::Private *Reason::ensureD()
    {
        if (!d) {
            d = new Private;
        }
        return d.data();
    }

    //----------------------------------------------------------------------------
    // ContentBase
    //----------------------------------------------------------------------------
    ContentBase::ContentBase(Origin creator, const QString &name) : creator(creator), name(name) { }

    ContentBase::ContentBase(const QDomElement &el)
    {
        static QMap<QString, Origin> sendersMap({ { QStringLiteral("initiator"), Origin::Initiator },
                                                  { QStringLiteral("none"), Origin::Both },
                                                  { QStringLiteral("responder"), Origin::Responder } });
        creator     = creatorAttr(el);
        name        = el.attribute(QLatin1String("name"));
        senders     = sendersMap.value(el.attribute(QLatin1String("senders")));
        disposition = el.attribute(QLatin1String("disposition")); // if empty, it's "session"
    }

    QDomElement ContentBase::toXml(QDomDocument *doc, const char *tagName, const QString &ns) const
    {
        if (!isValid()) {
            return QDomElement();
        }
        auto el = ns.isEmpty() ? doc->createElement(QLatin1String(tagName))
                               : doc->createElementNS(ns, QLatin1String(tagName));
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
            break;
        }

        if (!disposition.isEmpty() && disposition != QLatin1String("session")) {
            el.setAttribute(QLatin1String("disposition"),
                            disposition); // NOTE review how we can parse it some generic way
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
    // JTPush - Jingle Task
    //----------------------------------------------------------------------------
    class JTPush : public Task {
        Q_OBJECT

        QList<QString> externalManagers;
        QList<QString> externalSessions;

    public:
        JTPush(Task *parent) : Task(parent) { }

        ~JTPush() { }

        inline void addExternalManager(const QString &ns) { externalManagers.append(ns); }
        inline void forgetExternalSession(const QString &sid) { externalSessions.removeOne(sid); }
        inline void registerExternalSession(const QString &sid) { externalSessions.append(sid); }

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
            Jid     from(fromStr);
            if (jingle.action() == Action::SessionInitiate) {
                if (!client()->jingleManager()->isAllowedParty(from)
                    || (!jingle.initiator().isEmpty()
                        && !client()->jingleManager()->isAllowedParty(jingle.initiator()))) {
                    respondError(iq, Stanza::Error::Cancel, Stanza::Error::ServiceUnavailable);
                    return true;
                }

                Jid redirection(client()->jingleManager()->redirectionJid());
                if (redirection.isValid()) {
                    respondError(iq, Stanza::Error::Modify, Stanza::Error::Redirect,
                                 QStringLiteral("xmpp:") + redirection.full());
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
            auto          resp = createIQ(client()->doc(), "error", iq.attribute(QStringLiteral("from")),
                                 iq.attribute(QStringLiteral("id")));
            Stanza::Error error(errType, errCond, text);
            auto          errEl = error.toXml(*client()->doc(), client()->stream().baseNS());
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
            auto resp = createIQ(client()->doc(), "error", iq.attribute(QStringLiteral("from")),
                                 iq.attribute(QStringLiteral("id")));
            resp.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
            client()->send(resp);
        }
    };

    //----------------------------------------------------------------------------
    // SessionManagerPad - handle event related to a type of app/transport but not specific instance
    //----------------------------------------------------------------------------
    QDomElement SessionManagerPad::takeOutgoingSessionInfoUpdate() { return QDomElement(); }

    void SessionManagerPad::populateOutgoing(Action action, QDomElement &el)
    {
        Q_UNUSED(action);
        Q_UNUSED(el);
    }

    void SessionManagerPad::onLocalAccepted() { }
    void SessionManagerPad::onSend() { }

    QDomDocument *SessionManagerPad::doc() const { return session()->manager()->client()->doc(); }

    //----------------------------------------------------------------------------
    // Manager
    //----------------------------------------------------------------------------
    class Manager::Private {
    public:
        XMPP::Client *         client;
        Manager *              manager;
        QScopedPointer<JTPush> pushTask;
        // ns -> application
        std::map<QString, QPointer<ApplicationManager>> applicationManagers;
        // ns -> parser function
        std::map<QString, QPointer<TransportManager>> transportManagers;
        std::function<bool(const Jid &)>              remoteJidCecker;

        // when set/valid any incoming session initiate will be replied with redirection error
        Jid                                   redirectionJid;
        XMPP::Stanza::Error                   lastError;
        QHash<QPair<Jid, QString>, Session *> sessions;
        int                                   maxSessions = -1; // no limit

        void setupSession(Session *s)
        {
            QObject::connect(s, &Session::terminated, manager,
                             [this, s]() { sessions.remove(qMakePair(s->peer(), s->sid())); });
        }
    };

    Manager::Manager(Client *client) : QObject(client), d(new Private())
    {
        d->client  = client;
        d->manager = this;
        d->pushTask.reset(new JTPush(client->rootTask()));
        /*
        static bool mtReg = false;
        if (!mtReg) {
            qRegisterMetaType<Session>();
        }
        */
    }

    Manager::~Manager()
    {
        for (auto &m : d->transportManagers) {
            m.second->setJingleManager(nullptr);
        }
        for (auto &m : d->applicationManagers) {
            m.second->setJingleManager(nullptr);
        }
    }

    Client *Manager::client() const { return d->client; }

    void Manager::addExternalManager(const QString &ns) { d->pushTask->addExternalManager(ns); }

    void Manager::registerExternalSession(const QString &sid) { d->pushTask->registerExternalSession(sid); }

    void Manager::forgetExternalSession(const QString &sid) { d->pushTask->forgetExternalSession(sid); }

    void Manager::setRedirection(const Jid &to) { d->redirectionJid = to; }

    const Jid &Manager::redirectionJid() const { return d->redirectionJid; }

    void Manager::registerApplication(ApplicationManager *app)
    {
        auto const &nss = app->ns();
        for (auto const &ns : nss)
            d->applicationManagers.emplace(ns, app);
        app->setJingleManager(this);
    }

    void Manager::unregisterApp(const QString &ns)
    {
        auto node = d->applicationManagers.extract(ns);
        if (node) {
            node.mapped()->closeAll(ns);
        }
    }

    bool Manager::isRegisteredApplication(const QString &ns) { return d->applicationManagers.count(ns); }

    ApplicationManagerPad *Manager::applicationPad(Session *session, const QString &ns)
    {
        auto it = d->applicationManagers.find(ns);
        if (it == d->applicationManagers.end()) {
            return nullptr;
        }
        return it->second->pad(session);
    }

    void Manager::registerTransport(TransportManager *transport)
    {
        auto const &nss = transport->ns();
        for (auto const &ns : nss)
            d->transportManagers.emplace(ns, transport);
        transport->setJingleManager(this);
    }

    void Manager::unregisterTransport(const QString &ns)
    {
        auto trManager = d->transportManagers.extract(ns);
        if (trManager) {
            trManager.mapped()->closeAll(ns);
        }
    }

    bool Manager::isRegisteredTransport(const QString &ns) { return d->transportManagers.count(ns); }

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

    void Manager::setRemoteJidChecker(std::function<bool(const Jid &)> checker) { d->remoteJidCecker = checker; }

    TransportManagerPad *Manager::transportPad(Session *session, const QString &ns)
    {
        auto transportManager = d->transportManagers.find(ns);
        if (transportManager == d->transportManagers.end()) {
            return nullptr;
        }
        return transportManager->second->pad(session);
    }

    QStringList Manager::availableTransports(const TransportFeatures &features) const
    {
        std::vector<std::pair<int, QString>> prio;
        prio.reserve(d->transportManagers.size());
        for (auto const &[ns, m] : d->transportManagers) {
            if (m->canMakeConnection(features, ns))
                prio.emplace_back(m->features(), ns);
        }
        std::sort(prio.begin(), prio.end(), [](auto const &a, auto const &b) { return a.first < b.first; });
        QStringList nss;
        std::transform(prio.begin(), prio.end(), std::back_inserter(nss), [](auto const &p) { return p.second; });
        return nss;
        // sorting by features is totally unreliable, so we have TransportSelector to do better job
    }

    QStringList Manager::discoFeatures() const
    {
        QStringList ret;
        for (auto const &mgr : d->applicationManagers) {
            ret += mgr.second->discoFeatures();
        }
        for (auto const &mgr : d->transportManagers) {
            ret += mgr.second->discoFeatures();
        }
        return ret;
    }

    Session *Manager::incomingSessionInitiate(const Jid &from, const Jingle &jingle, const QDomElement &jingleEl)
    {
        if (d->maxSessions > 0 && d->sessions.size() == d->maxSessions) {
            d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Wait, XMPP::Stanza::Error::ResourceConstraint);
            return nullptr;
        }
        auto key = qMakePair(from, jingle.sid());
        auto s   = new Session(this, from, Origin::Responder);
        if (s->incomingInitiate(jingle, jingleEl)) { // if parsed well
            d->sessions.insert(key, s);
            d->setupSession(s);
            // emit incomingSession makes sense when there are no unsolved conflicts in content descriptions /
            // transports
            // QTimer::singleShot(0,[s, this](){ emit incomingSession(s); });
            // QMetaObject::invokeMethod(this, "incomingSession", Qt::QueuedConnection, Q_ARG(Session *, s));
            QTimer::singleShot(0, this, [s, this]() { emit incomingSession(s); });
            return s;
        }
        d->lastError = s->lastError();
        delete s;
        return nullptr;
    }

    XMPP::Stanza::Error Manager::lastError() const { return d->lastError; }

    Session *Manager::newSession(const Jid &j)
    {
        auto s = new Session(this, j);
        d->setupSession(s);
        return s;
    }

    QString Manager::registerSession(Session *session)
    {
        QString id;
        auto    peer = session->peer();
        do {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
            id = QString("%1").arg(QRandomGenerator::global()->generate(), 6, 32, QChar('0'));
#else
            id = QString("%1").arg(quint32(qrand()), 6, 32, QChar('0'));
#endif
        } while (d->sessions.contains(qMakePair(peer, id)));
        d->sessions.insert(qMakePair(peer, id), session);
        return id;
    }

    //----------------------------------------------------------------------------
    // ErrorUtil
    //----------------------------------------------------------------------------
    const char *ErrorUtil::names[ErrorUtil::Last]
        = { "out-of-order", "tie-break", "unknown-session", "unsupported-info" };

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
