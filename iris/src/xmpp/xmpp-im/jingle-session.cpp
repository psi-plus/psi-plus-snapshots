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
#include "jingle-group-negotiation_p.h"
#include "xmpp/jid/jid.h"
#include "xmpp_caps.h"
#include "xmpp_client.h"
#include "xmpp_task.h"
#include "xmpp_xmlcommon.h"

#include <QPointer>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <vector>

template <class T> constexpr std::add_const_t<T> &as_const(T &t) noexcept { return t; }

#if QT_VERSION < QT_VERSION_CHECK(5, 7, 0)
// this adds const to non-const objects (like std::as_const)
template <typename T> Q_DECL_CONSTEXPR typename std::add_const<T>::type &std::as_const(T &t) noexcept { return t; }
// prevent rvalue arguments:
template <typename T> void std::as_const(const T &&) = delete;
#endif

namespace XMPP { namespace Jingle {
    //----------------------------------------------------------------------------
    // JT - Jingle Task
    //----------------------------------------------------------------------------
    class JT : public Task {
        Q_OBJECT

        QDomElement iq_;
        Jid         to_;

    public:
        JT(Task *parent) : Task(parent) { }

        ~JT() { }

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
        std::optional<XMPP::Stanza::Error>                 lastError;
        Reason                                             terminateReason;
        QMap<QString, QWeakPointer<ApplicationManagerPad>> applicationPads;
        QMap<QString, QWeakPointer<TransportManagerPad>>   transportPads;
        QMap<ContentKey, Application *>                    contentList;
        QSet<Application *>                                signalingContent;
        QList<ContentGroup>                                groups;
        QList<ContentGroup>                                remoteGroups;
        QList<ContentGroup>                                sentInitialGroups;

        // not yet acccepted applications from initial incoming request
        QList<Application *> initialIncomingUnacceptedContent;

        // session level updates. session-info for example or some rejected apps
        QHash<Action, OutgoingUpdate> outgoingUpdates;

        QString sid;
        Jid     origFrom;   // "from" attr of IQ.
        Jid     otherParty; // either "from" or initiator/responder. it's where to send all requests.
        Jid     localParty; // that one will be set as initiator/responder if provided
        bool    waitingAck      = false;
        bool    needNotifyGroup = false; // whenever grouping info changes
        bool    groupingAllowed = false;

        void setSessionFinished()
        {
            q->tieBreaker()->clear();
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

        QList<QDomElement> genGroupingXML()
        {
            QList<QDomElement> ret;
            if (!groupingAllowed)
                return ret;

            QDomDocument &doc = *manager->client()->doc();

            for (const auto &group : groups) {
                auto g = doc.createElementNS(QLatin1String("urn:xmpp:jingle:apps:grouping:0"), QLatin1String("group"));
                g.setAttribute(QLatin1String("semantics"), group.semantics);
                for (auto const &name : group.contents) {
                    auto c = doc.createElementNS(QLatin1String("urn:xmpp:jingle:apps:grouping:0"),
                                                 QLatin1String("content"));
                    c.setAttribute(QLatin1String("name"), name);
                    g.appendChild(c);
                }
                ret.append(g);
            }
            return ret;
        }

        template <void (SessionManagerPad::*func)()> void notifyPads()
        {
            for (auto &weakPad : transportPads) {
                auto pad = weakPad.lock();
                if (pad) {
                    (pad.data()->*func)(); // just calls pad's method
                }
            }
            for (auto &weakPad : applicationPads) {
                auto pad = weakPad.lock();
                if (pad) {
                    (pad.data()->*func)();
                }
            }
        }

        void sendJingle(Action action, QList<QDomElement> update,
                        std::function<void(JT *)> callback = std::function<void(JT *)>())
        {
            QDomDocument &doc = *manager->client()->doc();
            Jingle        jingle(action, sid);
            if (action == Action::SessionInitiate) {
                jingle.setInitiator(manager->client()->jid());
            }
            if (action == Action::SessionAccept) {
                jingle.setResponder(manager->client()->jid());
            }
            auto                xml = jingle.toXml(&doc);
            QList<ContentGroup> includedGroups;

            for (const QDomElement &e : update) {
                xml.appendChild(e);
            }
            if (needNotifyGroup
                && (action == Action::SessionInitiate || action == Action::SessionAccept || action == Action::ContentAdd
                    || action == Action::ContentAccept)) {
                const auto xmls = genGroupingXML();
                if (!xmls.isEmpty())
                    includedGroups = groups;
                for (auto const &g : xmls)
                    xml.appendChild(g);
                needNotifyGroup = false;
            }

            if (action == Action::SessionInitiate) {
                // Snapshot what was actually serialized (including capability
                // filtering), not the mutable local proposal used by the UI.
                sentInitialGroups = includedGroups;
            }
            auto jt = new JT(manager->client()->rootTask());
            jt->request(otherParty, xml);
            const auto tieBreakTransaction = q->tieBreaker()->outgoingStarted(action, xml);
            QObject::connect(jt, &JT::finished, q, [jt, callback, tieBreakTransaction, this]() {
                waitingAck = false;
                const auto error
                    = jt->success() ? std::optional<Stanza::Error>() : std::optional<Stanza::Error>(jt->error());
                QPointer<Session> session(q);
                q->tieBreaker()->outgoingFinished(tieBreakTransaction, error);
                if (!session)
                    return;
                if (callback)
                    callback(jt);
                if (!session)
                    return;
                if (!jt->success())
                    lastError = jt->error();
                q->tieBreaker()->outgoingCallbacksFinished(tieBreakTransaction);
                if (!session)
                    return;
                planStep();
            });
            waitingAck = true;
            jt->go(true);
        }

        void planStep()
        {
            if (waitingAck) {
                return;
            }
            lastError = {};
            if (!stepTimer.isActive()) {
                stepTimer.start();
            }
        }

        void doStep()
        {
            if (waitingAck || state == State::Finished) {
                // in waitingAck we will return here later
                qDebug("jingle-doStep: skip step: %s", waitingAck ? "waitingAck" : "session already finished");
                return;
            }

            if (terminateReason.condition() && state != State::Finished) {
                if (state != State::Created || role == Origin::Responder) {
                    sendJingle(Action::SessionTerminate,
                               QList<QDomElement>() << terminateReason.toXml(manager->client()->doc()));
                }
                setSessionFinished();
                qDebug("jingle-doStep: the step finished the session due to terminationReason previously set");
                return;
            }

            if (state == State::Created && role == Origin::Responder) {
                // we could fail very early if something went wrong with transports init for example
                Reason reason;
                bool   all = true;
                for (auto const &c : std::as_const(contentList)) {
                    if (c->state() < State::Finishing) {
                        all = false;
                        break;
                    }

                    if (c->state() == State::Finishing) {
                        auto upd = c->evaluateOutgoingUpdate();
                        if (upd.action == Action::ContentRemove && upd.reason.condition()) {
                            reason = upd.reason;
                        }
                    }
                }
                if (all) {
                    terminateReason = reason;
                    sendJingle(Action::SessionTerminate,
                               QList<QDomElement>() << terminateReason.toXml(manager->client()->doc()));
                    setSessionFinished();
                    qDebug("jingle-doStep: all apps finished -> session finished");
                    return;
                }
            }

            if (state == State::Created) {
                qDebug("jingle-doStep: still in Created state. exit");
                return; // should wait for user approval of send/accept
            }

            if (outgoingUpdates.size()) {
                auto it       = outgoingUpdates.begin();
                auto action   = it.key();
                auto updates  = it.value();
                auto elements = std::get<0>(updates);
                auto cb       = std::get<1>(updates);
                outgoingUpdates.erase(it);
                sendJingle(action, elements, cb);
                qDebug("jingle-doStep: sent outgoingUpdates");
                return;
            }

            QList<QDomElement> updateXml;
            for (auto &mp : applicationPads) {
                auto        p  = mp.toStrongRef();
                QDomElement el = p->takeOutgoingSessionInfoUpdate();
                if (!el.isNull()) {
                    updateXml.append(el);
                    // we can send session-info for just one application. so stop processing
                    sendJingle(Action::SessionInfo, updateXml, [](JT *jt) {
                        if (!jt->success())
                            qWarning("failure for session-info is ignored");
                    });
                    qDebug("jingle-doStep: sent session info");
                    return;
                }
            }

            typedef std::tuple<QPointer<Application>, OutgoingUpdateCB> AckHndl; // will be used from callback on iq ack
            if (state == State::ApprovedToSend) { // we are going to send session-initiate/accept (already accepted
                                                  // by the user but not sent yet)
                if (trySendSessionAcceptOrInitiate()) {
                    qDebug("jingle-doStep: session is not yet ready to be accepted/initiated");
                    return; // accepted / initiated or finished with a failure
                }
            }

            QMultiMap<Application::Update, Application *> updates;
            qDebug("jingle-doStep: %lld applications have updates", qsizetype(signalingContent.size()));
            for (auto app : std::as_const(signalingContent)) {
                auto updateType = app->evaluateOutgoingUpdate();
                if (updateType.action != Action::NoAction) {
                    if (state == State::ApprovedToSend && app->flags() & Application::InitialApplication) {
                        // We need pass here everthing not checked in trySendSessionAcceptOrInitiate
                        if ((role == Origin::Initiator && updateType.action == Action::ContentAdd)
                            || (role == Origin::Responder && updateType.action == Action::ContentAccept)) {
                            continue; // skip in favor of trySendSessionAcceptOrInitiate
                        }
                    }
                    updates.insert(updateType, app);
                }
            }

            QList<AckHndl> acceptApps;
            if (updates.size()) {
                auto       upd  = updates.begin().key(); // NOTE maybe some actions have more priority than others
                auto const apps = updates.values(upd);
                for (auto app : apps) {
                    QList<QDomElement> xml;
                    OutgoingUpdateCB   callback;
                    std::tie(xml, callback) = app->takeOutgoingUpdate();
                    updateXml += xml;
                    if (callback) {
                        acceptApps.append(AckHndl { app, callback });
                    }
                }
                sendJingle(upd.action, updateXml, [this, acceptApps](JT *jt) {
                    QPointer<Session> session(q);
                    for (const auto &h : acceptApps) {
                        auto app      = std::get<0>(h);
                        auto callback = std::get<1>(h);
                        if (app) {
                            callback(jt);
                            if (!session)
                                return;
                        }
                    }
                    planStep();
                });
            }
        }

        bool trySendSessionAcceptOrInitiate()
        {
            /*
             * For session-initiate everything is pretty much straightforward, just any content with
             * Action::ContentAdd update type has to be added. But with session-accept things are more complicated
             *   1. Local client could add its content. So we have to check content origin too.
             *   2. Remote client could add more content before local session-accept. Then we have two options
             *         a) send content-accept and skip this content in session-accept later
             *         b) don't send content-accept and accept everything with session-accept
             *      We prefer option (b) in our implementation.
             */
            typedef std::tuple<QPointer<Application>, OutgoingUpdateCB, bool> AckHndl;
            QSet<Application *>                                               rejectedInitialContent;
            if (role == Origin::Responder) {
                int    acceptedInitialContent = 0;
                Reason rejectionReason;
                for (const auto &c : std::as_const(initialIncomingUnacceptedContent)) {
                    auto out = c->evaluateOutgoingUpdate();
                    if (out.action == Action::ContentAccept) {
                        ++acceptedInitialContent;
                        continue;
                    }
                    if (out.action == Action::ContentReject || out.action == Action::ContentRemove) {
                        rejectedInitialContent.insert(c);
                        if (!rejectionReason.isValid() && out.reason.isValid())
                            rejectionReason = out.reason;
                        continue;
                    }
                    return false; // keep waiting.
                }
                if (!acceptedInitialContent) {
                    q->terminate(rejectionReason.isValid() ? rejectionReason.condition() : Reason::Decline,
                                 rejectionReason.text());
                    return true;
                }
            } else {
                for (const auto &c : std::as_const(contentList)) {
                    auto out = c->evaluateOutgoingUpdate();
                    if (out.action == Action::ContentRemove) {
                        lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                        XMPP::Stanza::Error::ErrorCond::BadRequest);
                        setSessionFinished();
                        return true;
                    }
                    if (out.action != Action::ContentAdd) {
                        return false; // keep waiting.
                    }
                }
            }
            Action actionToSend = Action::SessionAccept;
            State  finalState   = State::Active;
            // so all contents is ready for session-initiate. let's do it
            if (role == Origin::Initiator) {
                if (sid.isEmpty())
                    sid = manager->registerSession(q);
                actionToSend = Action::SessionInitiate;
                finalState   = State::Pending;
            }

            notifyPads<&SessionManagerPad::onSend>();

            // Pads may change the proposal in onSend(). Validate after that,
            // but before consuming application updates or entering Unacked.
            if (groupingAllowed && needNotifyGroup && !q->validLocalGroupings()) {
                lastError = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::BadRequest);
                q->terminate(Reason::IncompatibleParameters, QStringLiteral("Invalid local content grouping"));
                return true;
            }

            QList<QDomElement> contents;
            QList<AckHndl>     acceptApps;
            for (const auto &app : std::as_const(contentList)) {
                QList<QDomElement> xml;
                OutgoingUpdateCB   callback;
                std::tie(xml, callback)    = app->takeOutgoingUpdate();
                const bool rejectedInitial = role == Origin::Responder && rejectedInitialContent.contains(app);
                if (!rejectedInitial)
                    contents += xml;
                if (callback)
                    acceptApps.append(AckHndl { app, callback, !rejectedInitial });
            }
            if (contents.isEmpty()) {
                q->terminate(Reason::Decline, QStringLiteral("No initial content was accepted"));
                return true;
            }

            state = State::Unacked;
            initialIncomingUnacceptedContent.clear();
            sendJingle(actionToSend, contents, [this, acceptApps, finalState](JT *jt) {
                if (!jt->success()) {
                    qDebug("Session accept/initiate returned iq error");
                    emit q->terminated();
                    return;
                }
                state = finalState;
                for (const auto &h : acceptApps) {
                    auto app         = std::get<0>(h);
                    auto callback    = std::get<1>(h);
                    auto shouldStart = std::get<2>(h);
                    if (app) {
                        callback(jt);
                        if (role == Origin::Responder && shouldStart)
                            app->start();
                    }
                }
                if (finalState == State::Active) {
                    emit q->activated();
                }
                planStep();
            });

            return true;
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
                return TransportResult { false, Reason::NoReason, QSharedPointer<Transport>() };
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
            std::unique_ptr<Application> app(appPad->manager()->startApplication(appPad, c.name, c.creator, c.senders));
            if (!app)
                return result { Unparsed, Reason::Success, nullptr };

            auto descErr = app->setRemoteOffer(descriptionEl);
            if (descErr == Application::IncompatibleParameters) {
                return result { Unsupported, Reason::IncompatibleParameters, nullptr };
            } else if (descErr == Application::Unparsed) {
                return result { Unparsed, Reason::Success, nullptr };
            }

            if (app->setTransport(std::get<2>(trpr))) {
                return result { Ok, Reason::Success, app.release() };
            }
            // TODO We can do transport-replace in all cases where std::get<1>(trpr) != NoReason
            return result { Unsupported, Reason::IncompatibleParameters, app.release() };
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
                Application             *app;

                std::tie(err, cond, app) = parseContentAdd(ce);
                if (err == Private::AddContentError::Unparsed) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    qDeleteAll(addSet);
                    return ParseContentListResult(Unparsed, cond, QList<Application *>(), QList<QDomElement>());
                }

                auto contentName = ce.attribute(QLatin1String("name"));
                auto it          = addSet.find(contentName);
                if (err != Private::AddContentError::Ok) {
                    // can't continue as well
                    if (app) { // we are going to reject it completely so delete
                        delete app;
                    }

                    if (it == addSet.end()) {
                        // OutgoingUpdate outlives this incoming stanza. Import the
                        // rejected content into the client-owned document instead
                        // of retaining a handle into the parser-owned document.
                        auto owned = manager->client()->doc()->importNode(ce, true).toElement();
                        rejectSet.insert(contentName, std::make_pair(owned, cond));
                    } // else it was invalid alternative
                    continue;
                }

                rejectSet.remove(contentName);
                // REVIEW probably not wantBetterTransport but wantBetterApplication
                if (it == addSet.end() || (*it)->wantBetterTransport(app->transport())) {
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
                Application             *app;

                std::tie(err, cond, app) = parseContentAccept(ce);
                if (err == Private::AddContentError::Unparsed || err == Private::AddContentError::Unexpected) {
                    for (auto &a : acceptSet) {
                        a->setState(State::Pending); // reset state to pending for already passed validation before
                                                     // passing error back
                    }
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    err == Private::AddContentError::Unexpected
                                                        ? XMPP::Stanza::Error::ErrorCond::UnexpectedRequest
                                                        : XMPP::Stanza::Error::ErrorCond::BadRequest);
                    if (err == Private::AddContentError::Unexpected) {
                        ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
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
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return std::tuple<bool, QList<Application *>>(false, QList<Application *>());
                }

                if (err != Private::AddContentError::Ok) {
                    app->setState(State::Finished); // we can't keep working with this content for whatever reason. if
                                                    // "accept" failed there is no fallback
                    // This set is captured by a queued callback. Keep the XML
                    // in the client-owned document so the incoming stanza may die
                    // immediately after this handler returns.
                    auto owned = manager->client()->doc()->importNode(ce, true).toElement();
                    rejectSet.insert(
                        contentName,
                        std::make_pair(owned,
                                       cond)); // NOTE, probably instead of ce we have to generate original description
                    continue;
                }
                acceptSet.insert(contentName, app);
            }

            if (rejectSet.size()) {
                QTimer::singleShot(0, q, [this, rejectSet]() mutable {
                    auto               cond = rejectSet.first().second;
                    QList<QDomElement> rejects;
                    for (auto const &i : std::as_const(rejectSet)) {
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
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                if (err == Private::AddContentError::Unexpected) {
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
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
                for (auto app : std::as_const(apps)) {
                    addAndInitContent(remoteRole, app); // TODO check conflicts
                }
                QTimer::singleShot(0, q, [this]() { emit q->newContentReceived(); });
            }
            planStep();

            return true;
        }

        bool handleIncomingContentRemove(const QDomElement &jingleEl, bool rejected = false)
        {
            QSet<Application *> toRemove;
            QString             contentTag(QStringLiteral("content"));
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                ContentBase cb(ce);
                if (!cb.isValid()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                Application *app = contentList.value(ContentKey { cb.name, cb.creator });
                if (rejected && app
                    && (app->creator() != role || app->flags().testFlag(Application::InitialApplication)
                        || (app->state() != State::Pending && app->state() != State::Unacked))) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                    return false;
                }
                if (app) {
                    toRemove.insert(app);
                }
            }

            auto   reasonEl = jingleEl.firstChildElement(QString::fromLatin1("reason"));
            Reason reason = reasonEl.isNull() ? Reason(rejected ? Reason::Decline : Reason::Success) : Reason(reasonEl);

            for (auto app : toRemove) {
                signalingContent.remove(app);
                initialIncomingUnacceptedContent.removeAll(app);
                contentList.remove(ContentKey { app->contentName(), app->creator() });
                if (app->transport()) {
                    app->transport()->disconnect(app);
                    app->transport()->stop();
                }
                app->incomingRemove(reason);
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

        struct GuardedContent {
            QPointer<Application> application;
            ContentKey            key;
        };

        QList<GuardedContent> snapshotContents(const QList<Application *> &apps) const
        {
            QList<GuardedContent> guarded;
            guarded.reserve(apps.size());
            for (auto app : apps) {
                if (app)
                    guarded.append(GuardedContent { QPointer<Application>(app),
                                                    ContentKey { app->contentName(), app->creator() } });
            }
            return guarded;
        }

        void startAcceptedContents(QList<GuardedContent> guardedApps, bool notifyActivated = false)
        {
            if (guardedApps.isEmpty())
                return;
            // JTPush must send the acceptance IQ result before start() can send
            // transport traffic (in particular IBB <open/>, XEP-0261 section 2.1).
            QTimer::singleShot(
                0, q, [this, session = QPointer<Session>(q), guardedApps = std::move(guardedApps), notifyActivated]() {
                    for (const auto &entry : guardedApps) {
                        if (!session || state != State::Active)
                            return;
                        const auto app = entry.application;
                        if (!app || contentList.value(entry.key) != app.data() || app->state() != State::Accepted)
                            continue;

                        app->start();

                        // A callback may remove this content without cancelling
                        // the Session. Other accepted contents must still start.
                        if (!session || state != State::Active)
                            return;
                    }
                    if (contentList.isEmpty()) {
                        q->terminate(Reason::Success);
                        return;
                    }
                    if (notifyActivated) {
                        // Recheck the whole snapshot: a later start callback may
                        // have removed an earlier application. Never activate on
                        // behalf of a replacement object with the same content key.
                        for (const auto &entry : guardedApps) {
                            const auto app = entry.application;
                            if (app && contentList.value(entry.key) == app.data() && app->state() < State::Finishing) {
                                emit q->activated();
                                return;
                            }
                        }
                    }
                });
        }

        bool handleIncomingSessionAccept(const QDomElement &jingleEl)
        {
            auto peerGroups = Session::parseGroupings(jingleEl);
            if (!peerGroups || !Session::validBundleAnswer(sentInitialGroups, *peerGroups)) {
                lastError = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::BadRequest);
                return false;
            }
            bool                 parsed;
            QList<Application *> apps;

            std::tie(parsed, apps) = parseContentAcceptList(jingleEl);
            if (!parsed) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            // Snapshot every parser result before the first external callback.
            // Raw Application pointers returned by the parser are not stable:
            // stopping one omitted transport may synchronously delete a sibling,
            // an accepted content, or the Session itself.
            auto                guardedAccepted = snapshotContents(apps);
            QSet<Application *> accepted;
            for (const auto &entry : std::as_const(guardedAccepted)) {
                if (entry.application)
                    accepted.insert(entry.application.data());
            }

            QList<GuardedContent> omitted;
            for (auto it = contentList.cbegin(); it != contentList.cend(); ++it) {
                auto app = it.value();
                if (app->creator() == role && app->flags().testFlag(Application::InitialApplication)
                    && app->state() == State::Pending && !accepted.contains(app)) {
                    omitted.append(GuardedContent { QPointer<Application>(app), it.key() });
                }
            }

            if (!omitted.isEmpty() && guardedAccepted.isEmpty()) {
                lastError
                    = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::UnexpectedRequest);
                ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                return false;
            }

            QPointer<Session> session(q);
            const State       negotiationState = state;
            const Reason omittedReason(Reason::Decline, QStringLiteral("Initial content was not accepted by peer"));

            for (const auto &entry : std::as_const(omitted)) {
                if (!session)
                    return true;
                if (state != negotiationState)
                    return true; // a reentrant cancellation/termination wins

                auto application = entry.application;
                if (!application || contentList.value(entry.key) != application.data())
                    continue; // a previous callback already disposed of this sibling

                // Detach before invoking any external code. From this point the
                // current application is no longer owned by contentList, so this
                // scope must delete it even if the Session disappears.
                signalingContent.remove(application.data());
                initialIncomingUnacceptedContent.removeAll(application.data());
                contentList.remove(entry.key);

                const auto transport = application->transport();
                if (transport) {
                    transport->disconnect(application.data());
                    transport->stop();
                }

                if (!session) {
                    if (application)
                        delete application.data();
                    return true;
                }
                if (state != negotiationState) {
                    if (application)
                        delete application.data();
                    return true;
                }
                if (!application)
                    continue;

                application->incomingRemove(omittedReason);

                if (!session) {
                    if (application)
                        delete application.data();
                    return true;
                }
                if (state != negotiationState) {
                    if (application)
                        delete application.data();
                    return true;
                }
                if (application)
                    delete application.data();

                // Application destruction may itself synchronously dispose of
                // the Session or change its state through connected callbacks.
                if (!session)
                    return true;
                if (state != negotiationState)
                    return true;
            }

            if (!session)
                return true;
            if (state != negotiationState)
                return true;

            // Every accepted application must still be the same object under
            // the snapshotted key before committing Active. If an omitted-content
            // callback invalidated one, acknowledge the peer's stanza but locally
            // terminate instead of resurrecting a broken negotiation.
            for (const auto &entry : std::as_const(guardedAccepted)) {
                const auto application = entry.application;
                if (!application || contentList.value(entry.key) != application.data()
                    || application->state() != State::Accepted) {
                    q->terminate(Reason::Decline, QStringLiteral("Accepted content disappeared during session-accept"));
                    return true;
                }
            }

            remoteGroups = *peerGroups;
            // Session acceptance completes signaling, not transport connectivity.
            state = State::Active;
            startAcceptedContents(std::move(guardedAccepted), true);
            planStep();

            return true;
        }

        bool handleIncomingContentAccept(const QDomElement &jingleEl)
        {
            bool                 parsed;
            QList<Application *> apps;

            std::tie(parsed, apps) = parseContentAcceptList(jingleEl); // marks valid apps as accepted
            if (!parsed) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            auto guardedApps = snapshotContents(apps);
            if (!guardedApps.isEmpty() && state == State::Active) {
                startAcceptedContents(std::move(guardedApps));
            }
            planStep();

            return true;
        }

        // Prepared state belongs to this incoming IQ, never a mutable Session slot.
        // Validation precedes arbitration; advisory selection from a losing batch
        // happens only after its reply. Every provider callback is a lifetime boundary.
        bool handleIncomingTransportReplace(const QDomElement &jingleEl, std::function<void()> *afterReply)
        {
            struct ValidatedTransportReplace {
                QPointer<Application>     application;
                ContentKey                key;
                QWeakPointer<Transport>   current;
                quint64                   generation;
                QSharedPointer<Transport> incoming;
                QDomElement               content;
            };
            QPointer<Session>                  session(q);
            QVector<ValidatedTransportReplace> passed;
            QSet<ContentKey>                   toReject;
            QSet<ContentKey>                   seen;
            QString                            contentTag(QStringLiteral("content"));
            auto                               isCurrent = [session](const ValidatedTransportReplace &entry) {
                auto app       = entry.application;
                auto transport = entry.current.lock();
                return session && session->state() < State::Finishing && app && app->state() < State::Finishing
                    && session->content(entry.key.first, entry.key.second) == app.data() && transport
                    && app->transport() == transport && app->transportReplaceGeneration() == entry.generation;
            };
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                ContentBase cb(ce);
                if (!cb.isValid()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                const ContentKey key { cb.name, cb.creator };
                if (seen.contains(key)) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                seen.insert(key);
                if (seen.size() > 64) {
                    lastError
                        = Stanza::Error(Stanza::Error::ErrorType::Wait, Stanza::Error::ErrorCond::ResourceConstraint);
                    return false;
                }
                const auto transportEl = ce.firstChildElement(QStringLiteral("transport"));
                if (transportEl.isNull() || transportEl.namespaceURI().isEmpty()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }

                Application *app = contentList.value(key);
                if (!app || !app->transport() || app->state() >= State::Finishing
                    || (app->creator() == role && app->state() <= State::Unacked)) {
                    qDebug("not existing app or inaporpriate app state");
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::ItemNotFound);
                    return false;
                }

                passed.append(ValidatedTransportReplace { QPointer<Application>(app),
                                                          key,
                                                          app->transport().toWeakRef(),
                                                          app->transportReplaceGeneration(),
                                                          {},
                                                          ce.cloneNode(true).toElement() });
            }

            if (seen.isEmpty()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            const auto &negotiatedGroups = role == Origin::Initiator ? remoteGroups : groups;
            if (!GroupNegotiation::replacementBatchPreservesBundles(negotiatedGroups, seen)) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
                ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                return false;
            }

            // Instantiate only detached incoming transports. update() is still a
            // provider callback, not a pure parse: never retain raw owner pointers
            // across it, and do not mutate current transports during this pass.
            for (auto &entry : passed) {
                bool              parsed;
                Reason::Condition reason;
                std::tie(parsed, reason, entry.incoming) = parseIncomingTransport(entry.content);
                if (!session)
                    return false;
                if (!parsed) {
                    lastError = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                if (reason)
                    entry.incoming.clear(); // valid but unsupported: reject this content, not the entire IQ
            }

            const auto resolution = q->tieBreaker()->resolveIncoming(Action::TransportReplace, jingleEl);
            if (!session)
                return false;
            if (resolution.error) {
                lastError = resolution.error;
                return false;
            }
            const bool       doTieBreak = resolution.solution == TieBreaker::Solution::Break;
            QSet<ContentKey> competing;
            if (doTieBreak) {
                for (auto el = resolution.localData.firstChildElement(contentTag); !el.isNull();
                     el      = el.nextSiblingElement(contentTag)) {
                    const ContentBase content(el);
                    competing.insert(ContentKey { content.name, content.creator });
                }
            }

            QVector<ValidatedTransportReplace> eligible;
            for (const auto &entry : std::as_const(passed)) {
                if (doTieBreak && competing.contains(entry.key))
                    continue;
                if (!entry.incoming || !isCurrent(entry)) {
                    toReject.insert(entry.key);
                    continue;
                }
                auto       app        = entry.application;
                const bool canReplace = app->transportSelector()->canReplace(entry.current.lock(), entry.incoming);
                if (!session)
                    return false;
                if (!isCurrent(entry) || !canReplace) {
                    toReject.insert(entry.key);
                    continue;
                }
                const bool enabled = app->isTransportReplaceEnabled();
                if (!session)
                    return false;
                if (!isCurrent(entry) || !enabled) {
                    toReject.insert(entry.key);
                    continue;
                }
                eligible.append(entry);
            }

            QSet<ContentKey> eligibleKeys;
            for (const auto &entry : std::as_const(eligible))
                eligibleKeys.insert(entry.key);
            if (!GroupNegotiation::replacementBatchPreservesBundles(negotiatedGroups, eligibleKeys)) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
                ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                return false;
            }

            // No remote installation on Break. Keep only validated sibling hints
            // and revalidate them once again after sending the error stanza.
            auto finish = [session, isCurrent, entries = eligible, doTieBreak, id = resolution.id]() {
                if (!session)
                    return;
                if (doTieBreak) {
                    for (const auto &entry : entries) {
                        if (!isCurrent(entry))
                            continue;
                        const auto current = entry.current.lock();
                        if (current->creator() == session->role() && current->state() < State::Unacked)
                            continue;
                        entry.application->selectNextTransport(entry.incoming);
                    }
                }
                if (session)
                    session->tieBreaker()->incomingFinished(
                        id, doTieBreak ? TieBreaker::RemoteResult::Rejected : TieBreaker::RemoteResult::Applied);
            };

            if (doTieBreak) {
                lastError = ErrorUtil::makeTieBreak(*manager->client()->doc());
            } else {
                for (const auto &entry : std::as_const(eligible)) {
                    if (!isCurrent(entry) || !entry.application->setTransport(entry.incoming))
                        toReject.insert(entry.key);
                    if (!session)
                        return false;
                }

                if (toReject.size()) {
                    QList<QDomElement> rejectImported;
                    for (const auto &entry : std::as_const(passed)) {
                        if (toReject.contains(entry.key) && entry.application
                            && entry.application->state() < State::Finishing
                            && contentList.value(entry.key) == entry.application.data())
                            rejectImported.append(
                                manager->client()->doc()->importNode(entry.content, true).toElement());
                    }
                    if (!rejectImported.isEmpty())
                        outgoingUpdates.insert(Action::TransportReject,
                                               OutgoingUpdate { rejectImported, OutgoingUpdateCB() });
                }
                planStep();
            }
            if (afterReply)
                *afterReply = std::move(finish);
            else
                finish(); // direct handler callers have no wire reply; production JTPush always supplies the hook
            return !doTieBreak;
        }

        // transport-accept is an acknowledgement of a transport-replace signaling
        // transaction, not merely a transport state update. First validate identities
        // and prepare every payload without live mutation. Only a completely prepared
        // batch is allowed to enter the reentrant commit pass.
        bool handleIncomingTransportAccept(const QDomElement &jingleEl)
        {
            struct ValidatedTransportAccept {
                QPointer<Application>          application;
                ContentKey                     key;
                QWeakPointer<Transport>        current;
                quint64                        generation = 0;
                QDomElement                    transport;
                Transport::PreparedUpdatePtr   prepared;
            };
            QString                               contentTag(QStringLiteral("content"));
            std::vector<ValidatedTransportAccept> updates;
            QSet<ContentKey>                      seen;
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                ContentBase cb(ce);
                auto        transportEl = ce.firstChildElement(QString::fromLatin1("transport"));
                QString     transportNS = transportEl.namespaceURI();
                if (!cb.isValid() || transportEl.isNull() || transportNS.isEmpty()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                const ContentKey key { cb.name, cb.creator };
                if (seen.contains(key)) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                seen.insert(key);

                Application *app = contentList.value(key);
                if (!app || !app->transport() || app->transport()->creator() != role
                    || app->transport()->state() != State::Pending || transportNS != app->transport()->pad()->ns()) {
                    // Ignore an out-of-order acknowledgement exactly as before.
                    qInfo("ignore out of order transport-accept");
                    continue;
                }
                if (!app->transportReplaceInProgress()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                updates.push_back(ValidatedTransportAccept { QPointer<Application>(app),
                                                             key,
                                                             app->transport().toWeakRef(),
                                                             app->transportReplaceGeneration(),
                                                             transportEl,
                                                             {} });
            }

            if (seen.isEmpty()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            auto setPrepareError = [this](const Transport::PrepareUpdateResult &result) {
                if (result.error) {
                    lastError = *result.error;
                    return;
                }
                const auto condition = result.status == Transport::PrepareUpdateStatus::Unsupported
                    ? XMPP::Stanza::Error::ErrorCond::FeatureNotImplemented
                    : XMPP::Stanza::Error::ErrorCond::BadRequest;
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel, condition);
            };

            QPointer<Session> session(q);

            // Pure preparation pass. A provider that has not implemented staged
            // updates fails closed; falling back to update() here would reintroduce
            // the partial-commit bug this boundary exists to prevent.
            for (auto &entry : updates) {
                if (!session)
                    return true;
                auto app              = entry.application;
                auto currentTransport = entry.current.lock();
                if (!app || contentList.value(entry.key) != app.data() || !currentTransport
                    || app->transport() != currentTransport
                    || app->transportReplaceGeneration() != entry.generation
                    || !app->transportReplaceInProgress())
                    continue;

                auto prepared = currentTransport->prepareUpdate(entry.transport);
                if (!session)
                    return true;
                if (!prepared) {
                    setPrepareError(prepared);
                    return false;
                }
                entry.prepared = std::move(prepared.update);
            }

            // Commit is intentionally a separate pass. Each commit may reenter and
            // remove/supersede later siblings, so recheck key, transport and generation
            // immediately before applying the owned value.
            for (auto &entry : updates) {
                if (!session)
                    return true;
                if (!entry.prepared)
                    continue;
                auto app              = entry.application;
                auto currentTransport = entry.current.lock();
                if (!app || contentList.value(entry.key) != app.data() || !currentTransport
                    || app->transport() != currentTransport
                    || app->transportReplaceGeneration() != entry.generation
                    || !app->transportReplaceInProgress())
                    continue;

                if (!app->incomingTransportAccept(std::move(entry.prepared))) {
                    if (!session)
                        return true;
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
            }

            if (!session)
                return true;
            planStep();
            return true;
        }

        // A peer transport-reject is valid only for a local replacement already in
        // PendingTransportReplace::InProgress. Validate the complete batch before
        // selecting any fallback. selectNextTransport() and selector callbacks may be
        // reentrant, so each second-pass entry is tied to the Application/key/transport
        // snapshot that was validated in the first pass.
        bool handleIncomingTransportReject(const QDomElement &jingleEl)
        {
            struct ValidatedTransportReject {
                QPointer<Application>   application;
                ContentKey              key;
                QWeakPointer<Transport> current;
            };
            QVector<ValidatedTransportReject> updates;
            QSet<ContentKey>                  seen;
            for (auto ce = jingleEl.firstChildElement(QStringLiteral("content")); !ce.isNull();
                 ce      = ce.nextSiblingElement(QStringLiteral("content"))) {
                ContentBase cb(ce);
                auto        transportEl = ce.firstChildElement(QStringLiteral("transport"));
                if (!cb.isValid() || transportEl.isNull() || transportEl.namespaceURI().isEmpty()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                const ContentKey key { cb.name, cb.creator };
                if (seen.contains(key)) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                seen.insert(key);

                auto app = contentList.value(key);
                if (!app || !app->transport() || !app->transport()->isLocal() || !app->transportReplaceInProgress()
                    || transportEl.namespaceURI() != app->transport()->pad()->ns()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                    return false;
                }
                updates.append(
                    ValidatedTransportReject { QPointer<Application>(app), key, app->transport().toWeakRef() });
            }

            if (updates.isEmpty()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            QPointer<Session> session(q);
            for (const auto &entry : std::as_const(updates)) {
                if (!session)
                    return true;
                auto app              = entry.application;
                auto currentTransport = entry.current.lock();
                if (!app || contentList.value(entry.key) != app.data() || !currentTransport
                    || app->transport() != currentTransport)
                    continue; // superseded or removed by a previous reentrant callback

                if (!app->incomingTransportReject()) {
                    if (!session)
                        return true;
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                    return false;
                }
            }
            if (!session)
                return true;
            planStep();
            return true;
        }
        bool handleIncomingContentModify(const QDomElement &jingleEl)
        {
            QList<QPair<QPointer<Application>, Origin>> updates;
            QSet<Application *>                         seen;
            for (auto ce = jingleEl.firstChildElement(); !ce.isNull(); ce = ce.nextSiblingElement()) {
                const auto name = ce.localName().isEmpty() ? ce.tagName() : ce.localName();
                if (name != QLatin1String("content") || ce.namespaceURI() != jingleEl.namespaceURI())
                    continue;
                ContentBase cb(ce);
                if (!cb.isValid() || !ce.firstChildElement().isNull()) {
                    lastError = Stanza::Error(Stanza::Error::ErrorType::Modify, Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                auto app = contentList.value(ContentKey { cb.name, cb.creator });
                if (state >= State::Finishing || !app || app->state() >= State::Finishing
                    || (app->isLocal() && app->state() < State::Unacked) || seen.contains(app)) {
                    lastError
                        = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::UnexpectedRequest);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                    return false;
                }
                if (!app->supportsContentModify()) {
                    lastError = Stanza::Error(Stanza::Error::ErrorType::Cancel,
                                              Stanza::Error::ErrorCond::FeatureNotImplemented);
                    return false;
                }
                seen.insert(app);
                updates.append(qMakePair(QPointer<Application>(app), cb.senders));
            }
            if (updates.isEmpty()) {
                lastError = Stanza::Error(Stanza::Error::ErrorType::Modify, Stanza::Error::ErrorCond::BadRequest);
                return false;
            }
            // Validate the entire batch before notifying applications. A notification
            // may synchronously remove another content or even destroy the session.
            QPointer<Session> session(q);
            for (const auto &update : updates) {
                if (!session)
                    return true;
                if (update.first && update.first->state() < State::Finishing)
                    update.first->incomingContentModify(update.second);
            }
            return true;
        }

        bool handleIncomingDescriptionInfo(const QDomElement &jingleEl)
        {
            QList<QPair<QPointer<Application>, QDomElement>> updates;
            QSet<Application *>                              seen;
            for (auto ce = jingleEl.firstChildElement(QStringLiteral("content")); !ce.isNull();
                 ce      = ce.nextSiblingElement(QStringLiteral("content"))) {
                ContentBase cb(ce);
                auto        description = ce.firstChildElement(QStringLiteral("description"));
                auto        app         = contentList.value(ContentKey { cb.name, cb.creator });
                if (!cb.isValid() || description.isNull()
                    || !description.nextSiblingElement(QStringLiteral("description")).isNull()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Modify,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                if (!app || app->state() >= State::Finishing || seen.contains(app)) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::OutOfOrder);
                    return false;
                }
                if (description.namespaceURI() != app->pad()->ns()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::FeatureNotImplemented);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::UnsupportedInfo);
                    return false;
                }
                seen.insert(app);
                updates.append(qMakePair(QPointer<Application>(app), description));
            }
            if (updates.isEmpty()) {
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Modify,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }
            for (const auto &update : updates) {
                if (!update.first || !update.first->incomingDescriptionInfo(update.second)) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::FeatureNotImplemented);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::UnsupportedInfo);
                    return false;
                }
            }
            return true;
        }

        bool handleIncomingSessionInfo(const QDomElement &jingleEl)
        {
            ApplicationManagerPad::Ptr destination;
            bool                       hasElements = false;
            for (QDomElement child = jingleEl.firstChildElement(); !child.isNull();
                 child             = child.nextSiblingElement()) {
                hasElements = true;
                ApplicationManagerPad::Ptr owner;
                for (const auto &weakPad : std::as_const(applicationPads)) {
                    auto pad = weakPad.toStrongRef();
                    if (pad && pad->sessionInfoNamespaces().contains(child.namespaceURI())) {
                        if (owner && owner != pad) {
                            owner.clear(); // ambiguous namespace ownership
                            break;
                        }
                        owner = pad;
                    }
                }
                if (!owner || (destination && destination != owner)) {
                    lastError = Stanza::Error(Stanza::Error::ErrorType::Cancel,
                                              Stanza::Error::ErrorCond::FeatureNotImplemented);
                    ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::UnsupportedInfo);
                    return false;
                }
                destination = owner;
            }
            if (!hasElements && state >= State::ApprovedToSend) {
                return true;
            }
            if (destination) {
                QPointer<Session> guard(q);
                const bool        handled = destination->incomingSessionInfo(jingleEl);
                if (handled || !guard)
                    return handled;
            }
            lastError
                = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::FeatureNotImplemented);
            ErrorUtil::fill(jingleEl.ownerDocument(), *lastError, ErrorUtil::UnsupportedInfo);
            return false;
        }

        bool handleIncomingTransportInfo(const QDomElement &jingleEl)
        {
            struct ValidatedTransportInfo {
                QPointer<Application>         application;
                ContentKey                    key;
                QWeakPointer<Transport>       current;
                quint64                       generation = 0;
                QDomElement                   transport;
                Transport::PreparedUpdatePtr  prepared;
            };

            QString                             contentTag(QStringLiteral("content"));
            std::vector<ValidatedTransportInfo> updates;
            QSet<ContentKey>                    seen;
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                Application *app = nullptr;
                ContentBase  cb(ce);
                const ContentKey key { cb.name, cb.creator };
                if (!cb.isValid() || seen.contains(key) || !(app = q->content(cb.name, cb.creator))
                    || app->state() >= State::Finishing || !app->transport()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                seen.insert(key);

                auto tel = ce.firstChildElement(QStringLiteral("transport"));
                if (tel.isNull() || tel.namespaceURI() != app->transport()->pad()->ns()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                updates.push_back(ValidatedTransportInfo { QPointer<Application>(app),
                                                           key,
                                                           app->transport().toWeakRef(),
                                                           app->transportReplaceGeneration(),
                                                           tel,
                                                           {} });
            }

            auto setPrepareError = [this](const Transport::PrepareUpdateResult &result) {
                if (result.error) {
                    lastError = *result.error;
                    return;
                }
                const auto condition = result.status == Transport::PrepareUpdateStatus::Unsupported
                    ? XMPP::Stanza::Error::ErrorCond::FeatureNotImplemented
                    : XMPP::Stanza::Error::ErrorCond::BadRequest;
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel, condition);
            };

            QPointer<Session> session(q);
            for (auto &entry : updates) {
                if (!session)
                    return true;
                auto app              = entry.application;
                auto currentTransport = entry.current.lock();
                if (!app || contentList.value(entry.key) != app.data() || app->state() >= State::Finishing
                    || !currentTransport || app->transport() != currentTransport
                    || app->transportReplaceGeneration() != entry.generation)
                    continue;

                auto prepared = currentTransport->prepareUpdate(entry.transport);
                if (!session)
                    return true;
                if (!prepared) {
                    setPrepareError(prepared);
                    return false;
                }
                entry.prepared = std::move(prepared.update);
            }

            for (auto &entry : updates) {
                if (!session)
                    return true;
                if (!entry.prepared)
                    continue;
                auto app              = entry.application;
                auto currentTransport = entry.current.lock();
                if (!app || contentList.value(entry.key) != app.data() || app->state() >= State::Finishing
                    || !currentTransport || app->transport() != currentTransport
                    || app->transportReplaceGeneration() != entry.generation)
                    continue; // superseded by an earlier reentrant sibling commit

                if (!currentTransport->commitPreparedUpdate(std::move(entry.prepared))) {
                    if (!session)
                        return true;
                    lastError = currentTransport->lastError();
                    return false; // runtime failure should trigger transport replace
                }
            }

            return true;
        }
    };

    Session::Session(Manager *manager, const Jid &peer, Origin role) : d(new Private)
    {
        d->q               = this;
        d->role            = role;
        d->manager         = manager;
        d->otherParty      = peer;
        d->groupingAllowed = checkPeerCaps(QLatin1String("urn:ietf:rfc:5888"));
        d->stepTimer.setSingleShot(true);
        d->stepTimer.setInterval(0);
        connect(&d->stepTimer, &QTimer::timeout, this, [this]() { d->doStep(); });
        connect(manager->client(), &Client::disconnected, this, [this]() {
            d->waitingAck      = false;
            d->terminateReason = Reason(Reason::ConnectivityError, QLatin1String("local side disconnected"));
            d->setSessionFinished();
        });
    }

    Session::~Session()
    {
        // Application::destroyed removes entries from contentList. Detach the
        // list before deletion so those callbacks cannot invalidate iteration.
        const auto contents = d->contentList.values();
        d->contentList.clear();
        qDeleteAll(contents);
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

    Features Session::peerFeatures() const { return d->manager->client()->capsManager()->disco(peer()).features(); }

    bool Session::checkPeerCaps(const QString &ns) const
    {
        return d->manager->client()->capsManager()->disco(peer()).features().test(QStringList() << ns);
    }

    bool Session::isGroupingAllowed() const { return d->groupingAllowed; }

    std::optional<XMPP::Stanza::Error> Session::lastError() const { return d->lastError; }

    TieBreaker       *Session::tieBreaker() { return &tieBreaker_; }
    const TieBreaker *Session::tieBreaker() const { return &tieBreaker_; }

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

    void Session::setGrouping(const QString &groupType, const QStringList &group)
    {
        // Compatibility setter: replace all groups of these semantics. Use the
        // plural API to express multiple independently negotiated BUNDLE groups.
        auto groups = d->groups;
        groups.erase(std::remove_if(groups.begin(), groups.end(),
                                    [&groupType](const ContentGroup &g) { return g.semantics == groupType; }),
                     groups.end());
        if (!group.isEmpty())
            groups.append(ContentGroup { groupType, group });
        setGroupings(groups);
    }

    static bool validGroup(const ContentGroup &group)
    {
        if (group.semantics.trimmed().isEmpty())
            return false;
        QSet<QString> seen;
        for (const auto &name : group.contents) {
            if (name.isEmpty() || seen.contains(name))
                return false;
            seen.insert(name);
        }
        return true;
    }

    bool Session::setGroupings(const QList<ContentGroup> &groups)
    {
        for (const auto &group : groups) {
            if (!validGroup(group))
                return false;
        }
        // One content cannot belong to two BUNDLE transports. For the initial
        // answer, check membership against the peer's offer as well.
        if (!validBundleAnswer(groups, groups)
            || (d->role == Origin::Responder && d->state <= State::ApprovedToSend
                && !validBundleAnswer(d->remoteGroups, groups)))
            return false;
        d->groups          = groups;
        d->needNotifyGroup = true;
        return true;
    }

    QList<ContentGroup> Session::groupings() const { return d->groups; }
    QList<ContentGroup> Session::remoteGroupings() const { return d->remoteGroups; }

    bool Session::validLocalGroupings() const
    {
        if (!validBundleAnswer(d->groups, d->groups)
            || (d->role == Origin::Responder && !validBundleAnswer(d->remoteGroups, d->groups)))
            return false;
        QHash<QString, int> names;
        for (auto app : d->contentList) {
            if (app->state() < State::Finishing)
                ++names[app->contentName()];
        }
        for (const auto &group : d->groups) {
            for (const auto &name : group.contents) {
                if (names.value(name) != 1)
                    return false;
            }
        }
        return true;
    }

    bool Session::validBundleAnswer(const QList<ContentGroup> &offer, const QList<ContentGroup> &answer)
    {
        // Initial group membership only (RFC 9143 sections 7.3 and 7.4).
        // Transport attributes, bundle-only and tagged-content selection require
        // application/transport negotiation and are not validated here.
        QHash<QString, int> offered;
        for (int index = 0; index < offer.size(); ++index) {
            const auto &group = offer[index];
            if (group.semantics != QLatin1String("BUNDLE"))
                continue;
            for (const auto &name : group.contents) {
                if (name.isEmpty() || offered.contains(name))
                    return false;
                offered.insert(name, index);
            }
        }
        QSet<int>     answeredGroups;
        QSet<QString> answeredMembers;
        for (const auto &group : answer) {
            if (group.semantics != QLatin1String("BUNDLE") || group.contents.isEmpty())
                continue;
            const int index = offered.value(group.contents.first(), -1);
            if (index < 0 || answeredGroups.contains(index))
                return false;
            answeredGroups.insert(index);
            for (const auto &name : group.contents) {
                if (offered.value(name, -1) != index || answeredMembers.contains(name))
                    return false;
                answeredMembers.insert(name);
            }
        }
        return true;
    }

    std::optional<QList<ContentGroup>> Session::parseGroupings(const QDomElement &jingleEl)
    {
        const auto ns = QStringLiteral("urn:xmpp:jingle:apps:grouping:0");
        // Group names map to SDP mids, not (creator, name) content keys. A
        // reference must identify exactly one top-level content in this offer
        // or answer. Do not resolve it against an older local session snapshot.
        QHash<QString, int> contentNames;
        for (auto el = jingleEl.firstChildElement(); !el.isNull(); el = el.nextSiblingElement()) {
            if (el.namespaceURI() == QLatin1String("urn:xmpp:jingle:1") && el.localName() == QLatin1String("content")) {
                const auto name = el.attribute(QStringLiteral("name"));
                if (!name.isEmpty())
                    ++contentNames[name];
            }
        }
        QList<ContentGroup> groups;
        for (auto el = jingleEl.firstChildElement(); !el.isNull(); el = el.nextSiblingElement()) {
            if (el.namespaceURI() != ns || el.localName() != QLatin1String("group"))
                continue;
            ContentGroup group { el.attribute(QStringLiteral("semantics")), {} };
            for (auto child = el.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
                if (child.namespaceURI() != ns || child.localName() != QLatin1String("content"))
                    continue;
                group.contents.append(child.attribute(QStringLiteral("name")));
            }
            if (!validGroup(group))
                return std::nullopt;
            // RFC 5888 section 6: ignore the whole group if any reference is
            // unresolved. Keeping just its known members would change its meaning.
            const bool resolved
                = std::all_of(group.contents.cbegin(), group.contents.cend(),
                              [&contentNames](const QString &name) { return contentNames.value(name) == 1; });
            if (!resolved)
                continue;
            groups.append(group);
        }
        return groups;
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
        d->notifyPads<&SessionManagerPad::onLocalAccepted>();
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
            d->notifyPads<&SessionManagerPad::onLocalAccepted>();
            for (auto &c : d->contentList) {
                c->markInitialApplication(true);
                c->prepare();
            }
            d->planStep();
        }
    }

    QString Session::reserveSid(const QString &requestedSid)
    {
        if (d->role != Origin::Initiator || d->state != State::Created)
            return {};
        if (d->sid.isEmpty())
            d->sid = d->manager->registerSession(this, requestedSid);
        else if (!requestedSid.isEmpty() && d->sid != requestedSid)
            return {};
        return d->sid;
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
            auto deleter = [ns, session = QPointer<Session>(this)](TransportManagerPad *pad) {
                if (session)
                    session->d->transportPads.remove(ns);
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
            auto deleter = [ns, session = QPointer<Session>(this)](ApplicationManagerPad *pad) {
                if (session)
                    session->d->applicationPads.remove(ns);
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
        auto peerGroups = parseGroupings(jingleEl);
        if (!peerGroups) {
            d->lastError = Stanza::Error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::BadRequest);
            return false;
        }
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
            Q_ASSERT(!apps.isEmpty());
            d->remoteGroups                     = *peerGroups;
            d->initialIncomingUnacceptedContent = apps;
            for (auto app : std::as_const(apps)) {
                app->markInitialApplication(true);
                d->addAndInitContent(Origin::Initiator, app);
            }
            d->planStep();
            return true;
        }
        Q_ASSERT(false);
        return false; // unreachable
    }

    bool Session::updateFromXml(Action action, const QDomElement &jingleEl, std::function<void()> *afterReply)
    {
        if (d->state == State::Finished) {
            d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                               XMPP::Stanza::Error::ErrorCond::UnexpectedRequest);
            ErrorUtil::fill(jingleEl.ownerDocument(), *d->lastError, ErrorUtil::OutOfOrder);
            return false;
        }

        switch (action) {
        case Action::ContentAccept:
            return d->handleIncomingContentAccept(jingleEl);
        case Action::ContentAdd:
            return d->handleIncomingContentAdd(jingleEl);
        case Action::ContentModify:
            return d->handleIncomingContentModify(jingleEl);
        case Action::ContentReject:
            return d->handleIncomingContentRemove(jingleEl, true);
        case Action::ContentRemove:
            return d->handleIncomingContentRemove(jingleEl);
        case Action::DescriptionInfo:
            return d->handleIncomingDescriptionInfo(jingleEl);
        case Action::SecurityInfo:
            break;
        case Action::SessionAccept:
            return d->handleIncomingSessionAccept(jingleEl);
        case Action::SessionInfo:
            return d->handleIncomingSessionInfo(jingleEl);
        case Action::SessionInitiate: // impossible case. but let compiler be happy
            break;
        case Action::SessionTerminate:
            return d->handleIncomingSessionTerminate(jingleEl);
        case Action::TransportAccept:
            return d->handleIncomingTransportAccept(jingleEl);
        case Action::TransportInfo:
            return d->handleIncomingTransportInfo(jingleEl);
        case Action::TransportReject:
            return d->handleIncomingTransportReject(jingleEl);
        case Action::TransportReplace:
            return d->handleIncomingTransportReplace(jingleEl, afterReply);
        case Action::NoAction:
            break;
        }

        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                           XMPP::Stanza::Error::ErrorCond::FeatureNotImplemented);
        return false;
    }
}}

#include "jingle-session.moc"
