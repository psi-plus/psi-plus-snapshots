/*
 * jignle-session.cpp - Jingle Session
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

#include "jingle-session.h"

#include "jingle-application.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_task.h"
#include "xmpp_xmlcommon.h"

#include <QPointer>
#include <QTimer>

namespace XMPP { namespace Jingle {
    //----------------------------------------------------------------------------
    // JT - Jingle Task
    //----------------------------------------------------------------------------
    class JT : public Task {
        Q_OBJECT

        QDomElement iq_;
        Jid         to_;

    public:
        JT(Task *parent) : Task(parent) {}

        ~JT() {}

        void request(const Jid &to, const QDomElement &jingleEl)
        {
            to_ = to;
            iq_ = createIQ(doc(), "set", to.full(), id());
            iq_.appendChild(jingleEl);
        }

        void onGo() { send(iq_); }

        bool take(const QDomElement &x)
        {
            if (!iqVerify(x, to_, id()))
                return false;

            if (x.attribute("type") == "error") {
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
    class Session::Private {
    public:
        Session *q;
        Manager *manager;
        QTimer   stepTimer;
        State    state = State::Created; // state of session on our side. if it's incoming we start from Created anyaway
                                         // but Pending state is skipped
        Origin                                             role = Origin::Initiator; // my role in the session
        XMPP::Stanza::Error                                lastError;
        Reason                                             terminateReason;
        QMap<QString, QWeakPointer<ApplicationManagerPad>> applicationPads;
        QMap<QString, QWeakPointer<TransportManagerPad>>   transportPads;
        QMap<ContentKey, Application *>                    contentList;
        QSet<Application *>                                signalingContent;
        QList<Application *>
            initialIncomingUnacceptedContent; // not yet acccepted applications from initial incoming request

        // session level updates. session-info for example or some rejected apps
        QHash<Action, OutgoingUpdate> outgoingUpdates;

        QString sid;
        Jid     origFrom;   // "from" attr of IQ.
        Jid     otherParty; // either "from" or initiator/responder. it's where to send all requests.
        Jid     localParty; // that one will be set as initiator/responder if provided
        bool    waitingAck = false;

        void setSessionFinished()
        {
            state = State::Finished;
            emit q->terminated();
            signalingContent.clear();
            for (auto &c : contentList) {
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

        void sendJingle(Action action, QList<QDomElement> update,
                        std::function<void(bool)> callback = std::function<void(bool)>())
        {
            QDomDocument &doc = *manager->client()->doc();
            Jingle        jingle(action, sid);
            if (action == Action::SessionInitiate) {
                jingle.setInitiator(manager->client()->jid());
            }
            if (action == Action::SessionAccept) {
                jingle.setResponder(manager->client()->jid());
            }
            auto xml = jingle.toXml(&doc);

            for (const QDomElement &e : update) {
                xml.appendChild(e);
            }

            auto jt = new JT(manager->client()->rootTask());
            jt->request(otherParty, xml);
            QObject::connect(jt, &JT::finished, q, [jt, jingle, callback, this]() {
                waitingAck = false;
                if (callback) {
                    callback(jt->success());
                }
                if (jt->success()) {
                    planStep();
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

        void planStep()
        {
            if (waitingAck) {
                return;
            }
            lastError = Stanza::Error(0, 0);
            if (!stepTimer.isActive()) {
                stepTimer.start();
            }
        }

        void doStep()
        {
            if (waitingAck) { // we will return here when ack is received. Session::Unacked is possible also only with
                              // waitingAck
                return;
            }
            if (terminateReason.condition() && state != State::Finished) {
                if (state != State::Created || role == Origin::Responder) {
                    sendJingle(Action::SessionTerminate,
                               QList<QDomElement>() << terminateReason.toXml(manager->client()->doc()));
                }
                setSessionFinished();
                return;
            }
            if (state == State::Created || state == State::Finished) {
                return; // we will start doing something when initiate() is called
            }

            if (outgoingUpdates.size()) {
                auto it       = outgoingUpdates.begin();
                auto action   = it.key();
                auto updates  = it.value();
                auto elements = std::get<0>(updates);
                auto cb       = std::get<1>(updates);
                outgoingUpdates.erase(it);
                sendJingle(action, elements, [this, cb](bool success) {
                    if (cb) {
                        cb(success);
                    }
                    planStep();
                });
                return;
            }

            typedef std::tuple<QPointer<Application>, OutgoingUpdateCB> AckHndl; // will be used from callback on iq ack
            if (state == State::ApprovedToSend) { // we are going to send session-initiate/accept (already accepted
                                                  // by the user but not sent yet)
                /*
                 * For session-initiate everything is prety much straightforward, just any content with
                 * Action::ContentAdd update type has to be added. But with session-accept things are more complicated
                 *   1. Local client could add its content. So we have to check content origin too.
                 *   2. Remote client could add more content before local session-accept. Then we have two options
                 *         a) send content-accept and skip this content in session-accept later
                 *         b) don't send content-accept and accept everything with session-accept
                 *      We prefer option (b) in our implementation.
                 */
                if (role == Origin::Responder) {
                    for (const auto &c : initialIncomingUnacceptedContent) {
                        auto out = c->evaluateOutgoingUpdate();
                        if (out.action == Action::ContentReject) {
                            lastError
                                = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                            setSessionFinished();
                            return;
                        }
                        if (out.action != Action::ContentAccept) {
                            return; // keep waiting.
                        }
                    }
                } else {
                    for (const auto &c : contentList) {
                        auto out = c->evaluateOutgoingUpdate();
                        if (out.action == Action::ContentRemove) {
                            lastError
                                = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                            setSessionFinished();
                            return;
                        }
                        if (out.action != Action::ContentAdd) {
                            return; // keep waiting.
                        }
                    }
                }
                Action actionToSend = Action::SessionAccept;
                State  finalState   = State::Active;
                // so all contents is ready for session-initiate. let's do it
                if (role == Origin::Initiator) {
                    sid          = manager->registerSession(q);
                    actionToSend = Action::SessionInitiate;
                    finalState   = State::Pending;
                }

                QList<QDomElement> contents;
                QList<AckHndl>     acceptApps;
                for (const auto &app : contentList) {
                    QList<QDomElement> xml;
                    OutgoingUpdateCB   callback;
                    std::tie(xml, callback) = app->takeOutgoingUpdate();
                    contents += xml;
                    // p->setState(State::Unacked);
                    if (callback) {
                        acceptApps.append(AckHndl { app, callback });
                    }
                }
                state = State::Unacked;
                sendJingle(actionToSend, contents, [this, acceptApps, finalState](bool success) {
                    if (!success)
                        return; // TODO any error means session error. but for content-add error handling has to be
                                // moved out of sendJingle
                    state = finalState;
                    for (const auto &h : acceptApps) {
                        auto app      = std::get<0>(h);
                        auto callback = std::get<1>(h);
                        if (app) {
                            callback(true);
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
                auto        p  = mp.toStrongRef();
                QDomElement el = p->takeOutgoingSessionInfoUpdate();
                if (!el.isNull()) {
                    updateXml.append(el);
                    // we can send session-info for just one application. so stop processing
                    sendJingle(Action::SessionInfo, updateXml, [this](bool success) {
                        if (!success)
                            qWarning("failure for session-info is ignored");
                        planStep();
                    });
                    return;
                }
            }

            QMultiMap<Application::Update, Application *> updates;

            for (auto app : signalingContent) {
                auto updateType = app->evaluateOutgoingUpdate();
                if (updateType.action != Action::NoAction) {
                    updates.insert(updateType, app);
                }
            }

            QList<AckHndl> acceptApps;
            if (updates.size()) {
                auto upd  = updates.begin().key(); // NOTE maybe some actions have more priority than others
                auto apps = updates.values(upd);
                for (auto app : apps) {
                    QList<QDomElement> xml;
                    OutgoingUpdateCB   callback;
                    std::tie(xml, callback) = app->takeOutgoingUpdate();
                    updateXml += xml;
                    if (callback) {
                        acceptApps.append(AckHndl { app, callback });
                    }
                }
                sendJingle(upd.action, updateXml, [this, acceptApps](bool success) {
                    for (const auto &h : acceptApps) {
                        auto app      = std::get<0>(h);
                        auto callback = std::get<1>(h);
                        if (app) {
                            callback(success);
                        }
                    }
                    planStep();
                });
            }
        }

        Reason reason(const QDomElement &jingleEl)
        {
            QDomElement re = jingleEl.firstChildElement(QLatin1String("reason"));
            Reason      reason;
            if (!re.isNull()) {
                reason = Reason(re);
                if (!reason.isValid()) {
                    qDebug("invalid reason");
                }
            }
            return reason;
        }

        using TransportResult = std::tuple<bool, Reason::Condition, QSharedPointer<Transport>>;
        TransportResult parseIncomingTransport(const QDomElement &contentEl)
        {
            auto    tel = contentEl.firstChildElement(QLatin1String("transport"));
            QString transportNS;
            if (tel.isNull() || (transportNS = tel.namespaceURI()).isEmpty()) {
                TransportResult { false, Reason::NoReason, QSharedPointer<Transport>() };
            }
            auto trPad = q->transportPadFactory(transportNS);
            if (!trPad) {
                return TransportResult { true, Reason::UnsupportedTransports, QSharedPointer<Transport>() };
            }
            auto transport = trPad->manager()->newTransport(trPad, negateOrigin(role));
            if (transport && transport->update(tel)) {
                return TransportResult { true, Reason::NoReason, transport };
            }
            return TransportResult { false, Reason::NoReason, QSharedPointer<Transport>() };
        }

        void addAndInitContent(Origin creator, Application *content)
        {
            contentList.insert(ContentKey { content->contentName(), creator }, content);
            if (state != State::Created && content->evaluateOutgoingUpdate().action != Action::NoAction) {
                signalingContent.insert(content);
            }
            QObject::connect(content, &Application::updated, q, [this, content]() {
                signalingContent.insert(content);
                planStep();
            });
            QObject::connect(content, &Application::destroyed, q, [this, content]() {
                signalingContent.remove(content);
                initialIncomingUnacceptedContent.removeOne(content);
                for (auto it = contentList.begin(); it != contentList.end(); ++it) { // optimize for large lists?
                    if (it.value() == content) {
                        contentList.erase(it);
                        break;
                    }
                }
            });
        }

        enum AddContentError { Ok, Unparsed, Unexpected, Unsupported };

        std::tuple<AddContentError, Reason::Condition, Application *> parseContentAdd(const QDomElement &ce)
        {
            QDomElement descriptionEl = ce.firstChildElement(QLatin1String("description"));
            QString     descriptionNS = descriptionEl.namespaceURI();
            typedef std::tuple<AddContentError, Reason::Condition, Application *> result;

            ContentBase c(ce);
            auto        trpr = parseIncomingTransport(ce);
            if (!c.isValid() || descriptionEl.isNull() || descriptionNS.isEmpty() || !std::get<0>(trpr)) {
                return result { Unparsed, Reason::Success, nullptr };
            }

            auto appPad   = q->applicationPadFactory(descriptionNS);
            auto trReason = std::get<1>(trpr);
            if (!appPad || trReason != Reason::NoReason) {
                return result { Unsupported, trReason == Reason::NoReason ? Reason::UnsupportedApplications : trReason,
                                nullptr };
            }
            QScopedPointer<Application> app(appPad->manager()->startApplication(appPad, c.name, c.creator, c.senders));

            auto descErr = app->setRemoteOffer(descriptionEl);
            if (descErr == Application::IncompatibleParameters) {
                return result { Unsupported, Reason::IncompatibleParameters, nullptr };
            } else if (descErr == Application::Unparsed) {
                return result { Unparsed, Reason::Success, nullptr };
            }

            if (app->setTransport(std::get<2>(trpr))) {
                return result { Ok, Reason::Success, app.take() };
            }
            // TODO We can do transport-replace in all cases where std::get<1>(trpr) != NoReason
            return result { Unsupported, Reason::IncompatibleParameters, app.take() };
        }

        typedef std::tuple<AddContentError, Reason::Condition, QList<Application *>, QList<QDomElement>>
            ParseContentListResult;

        ParseContentListResult parseContentAddList(const QDomElement &jingleEl)
        {
            QMap<QString, Application *>                             addSet;
            QMap<QString, std::pair<QDomElement, Reason::Condition>> rejectSet;

            QString contentTag(QStringLiteral("content"));
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {

                Private::AddContentError err;
                Reason::Condition        cond;
                Application *            app;

                std::tie(err, cond, app) = parseContentAdd(ce);
                if (err == Private::AddContentError::Unparsed) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    qDeleteAll(addSet);
                    return ParseContentListResult(Unparsed, cond, QList<Application *>(), QList<QDomElement>());
                }

                auto contentName = app->contentName();
                auto it          = addSet.find(contentName);
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
                if (it == addSet.end()
                    || (*it)->wantBetterTransport(
                        app->transport())) { // probably not wantBetterTransport but wantBetterApplication
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
                for (auto const &i : rejectSet) {
                    rejectList.append(i.first);
                }
                return ParseContentListResult(Unsupported, rejectSet.first().second, addSet.values(), rejectList);
            }

            return ParseContentListResult(Ok, Reason::Success, addSet.values(), QList<QDomElement>());
        }

        std::tuple<AddContentError, Reason::Condition, Application *> parseContentAccept(const QDomElement &ce)
        {
            QDomElement descriptionEl = ce.firstChildElement(QLatin1String("description"));
            QDomElement transportEl   = ce.firstChildElement(QLatin1String("transport"));
            QString     descriptionNS = descriptionEl.namespaceURI();
            QString     transportNS   = transportEl.namespaceURI();
            typedef std::tuple<AddContentError, Reason::Condition, Application *> result;

            ContentBase c(ce);
            if (!c.isValid() || role != c.creator || descriptionEl.isNull() || transportEl.isNull()
                || descriptionNS.isEmpty() || transportNS.isEmpty()) {
                return result { Unparsed, Reason::NoReason, nullptr };
            }

            auto app = q->content(c.name, role);
            if (!(app && app->state() == State::Pending)) { // reaccept is possible
                return result { AddContentError::Unexpected, Reason::NoReason, app };
            }

            if (app->pad()->ns() != descriptionNS || app->transport()->pad()->ns() != transportNS) {
                // well it's more than unexpected. let's send unparsed
                return result { AddContentError::Unparsed, Reason::NoReason, app };
            }

            if (!app->transport()->update(transportEl)) {
                // clearly unparsed. otherwise the app will generate failure event with a Reason.
                return result { AddContentError::Unparsed, Reason::NoReason, app };
            }

            auto ansret = app->setRemoteAnswer(descriptionEl);
            if (ansret == Application::Unparsed)
                return result { AddContentError::Unparsed, Reason::NoReason, app };

            if (ansret == Application::IncompatibleParameters || app->state() != State::Accepted) {
                // parsed but was not accepted. so it's somehow incompatible
                return result { AddContentError::Unsupported, Reason::IncompatibleParameters, app };
            }

            return result { AddContentError::Ok, Reason::Success, app };
        }

        std::tuple<bool, QList<Application *>> parseContentAcceptList(const QDomElement &jingleEl)
        {
            QMap<QString, Application *>                             acceptSet;
            QMap<QString, std::pair<QDomElement, Reason::Condition>> rejectSet;

            QString contentTag(QStringLiteral("content"));
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {

                Private::AddContentError err;
                Reason::Condition        cond;
                Application *            app;

                std::tie(err, cond, app) = parseContentAccept(ce);
                if (err == Private::AddContentError::Unparsed || err == Private::AddContentError::Unexpected) {
                    for (auto &a : acceptSet) {
                        a->setState(State::Pending); // reset state to pending for already passed validation before
                                                     // passing error back
                    }
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel,
                                                    err == Private::AddContentError::Unexpected
                                                        ? XMPP::Stanza::Error::UnexpectedRequest
                                                        : XMPP::Stanza::Error::BadRequest);
                    if (err == Private::AddContentError::Unexpected) {
                        ErrorUtil::fill(jingleEl.ownerDocument(), lastError, ErrorUtil::OutOfOrder);
                    }
                    return std::tuple<bool, QList<Application *>>(false, QList<Application *>());
                }

                auto contentName = app->contentName();
                auto it          = acceptSet.find(contentName);
                auto rit         = rejectSet.find(contentName);
                if (it != acceptSet.end() || rit != rejectSet.end()) {
                    // duplicates are not allowed in accept request
                    for (auto &a : acceptSet) {
                        a->setState(State::Pending); // reset state to pending for already passed validation before
                                                     // passing error back
                    }
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    return std::tuple<bool, QList<Application *>>(false, QList<Application *>());
                }

                if (err != Private::AddContentError::Ok) {
                    app->setState(State::Finished); // we can't keep working with this content for whatever reason. if
                                                    // "accept" failed there is no fallback
                    rejectSet.insert(
                        contentName,
                        std::make_pair(ce,
                                       cond)); // NOTE, probably instead of ce we have to generate original description
                    continue;
                }
                acceptSet.insert(contentName, app);
            }

            if (rejectSet.size()) {
                QTimer::singleShot(0, q, [this, rejectSet]() mutable {
                    auto               cond = rejectSet.first().second;
                    QList<QDomElement> rejects;
                    for (auto const &i : rejectSet) {
                        rejects.append(i.first);
                    }
                    rejects += Reason(cond).toXml(manager->client()->doc());
                    outgoingUpdates.insert(Action::ContentRemove,
                                           OutgoingUpdate { rejects, [this, rejects](bool) {
                                                               for (auto &r : rejects) {
                                                                   ContentBase c(r);
                                                                   delete contentList.take(ContentKey { c.name, role });
                                                               }
                                                               if (contentList.isEmpty()) {
                                                                   // the other party has to generate session-terminate
                                                                   // but we do not care already
                                                                   setSessionFinished();
                                                               }
                                                           } });
                });
            }
            return std::tuple<bool, QList<Application *>>(true, acceptSet.values());
        }

        bool handleIncomingContentAdd(const QDomElement &jingleEl)
        {
            Private::AddContentError err;
            Reason::Condition        cond;
            QList<Application *>     apps;
            QList<QDomElement>       rejects;

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
                outgoingUpdates.insert(Action::ContentReject, OutgoingUpdate { rejects, OutgoingUpdateCB() });
                break;
            case Private::AddContentError::Ok:
                break;
            }

            if (apps.size()) {
                Origin remoteRole = negateOrigin(role);
                for (auto app : apps) {
                    addAndInitContent(remoteRole, app); // TODO check conflicts
                }
                QTimer::singleShot(0, q, [this]() { emit q->newContentReceived(); });
            }
            planStep();

            return true;
        }

        bool handleIncomingContentRemove(const QDomElement &jingleEl)
        {
            QSet<Application *> toRemove;
            QString             contentTag(QStringLiteral("content"));
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                ContentBase cb(ce);
                if (!cb.isValid()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    return false;
                }
                Application *app = contentList.value(ContentKey { cb.name, cb.creator });
                if (app) {
                    toRemove.insert(app);
                }
            }

            auto   reasonEl = jingleEl.firstChildElement(QString::fromLatin1("reason"));
            Reason reason   = reasonEl.isNull() ? Reason(Reason::Success) : Reason(reasonEl);

            for (auto app : toRemove) {
                app->incomingRemove(reason);
                contentList.remove(ContentKey { app->contentName(), app->creator() });
                delete app;
            }

            if (contentList.isEmpty()) {
                terminateReason = reason;
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
            bool                 parsed;
            QList<Application *> apps;

            std::tie(parsed, apps) = parseContentAcceptList(jingleEl);
            if (!parsed) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }

            state = State::Connecting;
            if (apps.size()) {
                for (auto app : apps) {
                    app->start();
                }
            }
            QTimer::singleShot(0, q, [this]() { emit q->activated(); });
            planStep();

            return true;
        }

        bool handleIncomingContentAccept(const QDomElement &jingleEl)
        {
            bool                 parsed;
            QList<Application *> apps;

            std::tie(parsed, apps) = parseContentAcceptList(jingleEl); // marks valid apps as accepted
            if (!parsed) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                return false;
            }

            if (apps.size() && state >= State::Active) {
                for (auto app : apps) {
                    app->start(); // start accepted app. connection establishing and data transfer are inside
                }
            }
            planStep();

            return true;
        }

        bool handleIncomingTransportReplace(const QDomElement &jingleEl)
        {
            QList<std::tuple<Application *, QSharedPointer<Transport>, QDomElement>> passed;
            QList<QDomElement>                                                       toReject;
            QString contentTag(QStringLiteral("content"));
            bool    doTieBreak = false;
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {

                ContentBase               cb(ce);
                bool                      transportParsed;
                Reason::Condition         errReason;
                QSharedPointer<Transport> transport;
                std::tie(transportParsed, errReason, transport) = parseIncomingTransport(ce);

                if (!cb.isValid() || !transportParsed) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    return false;
                }
                Application *app = contentList.value(ContentKey { cb.name, cb.creator });
                if (!app || (app->creator() == role && app->state() <= State::Unacked)) {
                    qDebug("not existing app or inaporpriate app state");
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::ItemNotFound);
                    return false;
                }

                if (errReason) {
                    qDebug("failed to construct transport");
                    toReject.append(ce);
                    continue;
                }

                auto old = app->transport();
                Q_ASSERT(old != nullptr);
                // if it's my transport and it's sent but unacknowledged but has to be accepted
                if (old->creator() == q->peerRole() && old->state() == State::Unacked && role == Origin::Initiator) {
                    doTieBreak = true;
                    continue;
                }

                passed.append(std::make_tuple(app, transport, ce));
            }

            for (auto &v : passed) {
                Application *             app;
                QSharedPointer<Transport> transport;
                QDomElement               ce;
                std::tie(app, transport, ce) = v;
                if (doTieBreak) {
                    if (app->transport()->creator() == role && app->transport()->state() < State::Unacked)
                        continue; // it will send transport soon
                    app->selectNextTransport(transport->pad()->ns());
                } else if (!app->incomingTransportReplace(transport)) {
                    // app should generate transport accept eventually. content-accept will
                    // work too if the content wasn't accepted yet
                    toReject.append(ce);
                }
            }

            if (doTieBreak) {
                lastError = ErrorUtil::makeTieBreak(*manager->client()->doc());
                return false;
            } else if (toReject.size()) {
                outgoingUpdates.insert(Action::TransportReject, OutgoingUpdate { toReject, OutgoingUpdateCB() });
            }

            planStep();
            return true;
        }

        bool handleIncomingTransportAccept(const QDomElement &jingleEl)
        {
            QString                                  contentTag(QStringLiteral("content"));
            QList<QPair<Application *, QDomElement>> updates;
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                ContentBase cb(ce);
                auto        transportEl = ce.firstChildElement(QString::fromLatin1("transport"));
                QString     transportNS = transportEl.namespaceURI();
                if (!cb.isValid() || transportEl.isNull() || transportNS.isEmpty()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::BadRequest);
                    return false;
                }

                Application *app = contentList.value(ContentKey { cb.name, cb.creator });
                if (!app || !app->transport() || app->transport()->creator() != role
                    || app->transport()->state() != State::Pending) {
                    // ignore out of order
                    continue;
                }
                updates.append(qMakePair(app, transportEl));
            }

            for (auto &u : updates) {
                if (u.first->transport()->update(u.second) && u.first->state() >= State::Connecting) {
                    u.first->transport()->start();
                }
                // if update fails transport should trigger replace procedure
            }

            planStep();
            return true;
        }

        bool handleIncomingTransportInfo(const QDomElement &jingleEl)
        {
            QString                                              contentTag(QStringLiteral("content"));
            QList<QPair<QSharedPointer<Transport>, QDomElement>> updates;
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                Application *app = nullptr;
                ContentBase  cb(ce);
                if (!cb.isValid() || !(app = q->content(cb.name, cb.creator)) || app->state() >= State::Finishing
                    || !app->transport()) {
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

            for (auto &u : updates) {
                u.first->update(u.second); // failure should trigger transport replace
            }

            return true;
        }
    };

    Session::Session(Manager *manager, const Jid &peer, Origin role) : d(new Private)
    {
        d->q          = this;
        d->role       = role;
        d->manager    = manager;
        d->otherParty = peer;
        d->stepTimer.setSingleShot(true);
        d->stepTimer.setInterval(0);
        connect(&d->stepTimer, &QTimer::timeout, this, [this]() { d->doStep(); });
    }

    Session::~Session()
    {
        qDeleteAll(d->contentList);
        qDebug("session %s destroyed", qPrintable(d->sid));
    }

    Manager *Session::manager() const { return d->manager; }
    State    Session::state() const { return d->state; }
    Jid      Session::me() const { return d->localParty; }
    Jid      Session::peer() const { return d->otherParty; }

    Jid Session::initiator() const
    {
        return d->role == Origin::Initiator ? d->manager->client()->jid() : d->otherParty;
    }

    Jid Session::responder() const
    {
        return d->role == Origin::Responder ? d->manager->client()->jid() : d->otherParty;
    }

    QString Session::sid() const { return d->sid; }

    Origin Session::role() const { return d->role; }

    Origin Session::peerRole() const { return negateOrigin(d->role); }

    XMPP::Stanza::Error Session::lastError() const { return d->lastError; }

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
        return d->contentList.value(ContentKey { contentName, creator });
    }

    void Session::addContent(Application *content)
    {
        Q_ASSERT(d->state < State::Finishing);
        d->addAndInitContent(d->role, content);
        if (d->state >= State::ApprovedToSend) {
            // If we add content to already initiated session then we are gonna
            // send it immediatelly. So start prepare
            content->prepare();
        }
    }

    const QMap<ContentKey, Application *> &Session::contentList() const { return d->contentList; }

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
            return pad->manager()->newTransport(pad, d->role); // pad on both side becaue we need shared pointer
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

    QStringList Session::allApplicationTypes() const { return d->applicationPads.keys(); }

    void Session::setLocalJid(const Jid &jid) { d->localParty = jid; }

    void Session::accept()
    {
        Q_ASSERT(d->role == Origin::Responder && d->state == State::Created);
        // So we presented a user incoming session in UI, the user modified it somehow and finally accepted.
        d->state = State::ApprovedToSend;
        for (auto &c : d->contentList) {
            c->prepare();
        }
        d->planStep();
    }

    void Session::initiate()
    {
        emit initiated();
        if (d->role == Origin::Initiator && d->state == State::Created) {
            d->state = State::ApprovedToSend;
            for (auto &c : d->contentList) {
                c->prepare();
            }
            d->planStep();
        }
    }

    void Session::terminate(Reason::Condition cond, const QString &comment)
    {
        if (d->role == Origin::Initiator && d->state == State::ApprovedToSend) {
            d->setSessionFinished();
            return;
        }
        d->state           = State::Finishing;
        d->terminateReason = Reason(cond, comment);
        d->planStep();
    }

    TransportManagerPad::Ptr Session::transportPadFactory(const QString &ns)
    {
        auto pad = d->transportPads.value(ns).toStrongRef();
        if (!pad) {
            auto deleter = [ns, this](TransportManagerPad *pad) {
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
            auto deleter = [ns, this](ApplicationManagerPad *pad) {
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
        d->sid      = jingle.sid();
        d->origFrom = d->otherParty;
        if (jingle.initiator().isValid() && !jingle.initiator().compare(d->origFrom)) {
            d->otherParty = jingle.initiator();
        }

        Private::AddContentError err;
        Reason::Condition        cond;
        QList<Application *>     apps;
        QList<QDomElement>       rejects;

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
            if (!apps.size())
                return false;
            d->initialIncomingUnacceptedContent = apps;
            for (auto app : apps) {
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
            break;
        }

        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::Cancel, XMPP::Stanza::Error::FeatureNotImplemented);
        return false;
    }
}}

#include "jingle-session.moc"
