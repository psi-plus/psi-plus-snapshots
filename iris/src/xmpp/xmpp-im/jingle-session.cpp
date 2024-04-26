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
#include "xmpp_caps.h"
#include "xmpp_client.h"
#include "xmpp_task.h"
#include "xmpp_xmlcommon.h"

#include <QPointer>
#include <QTimer>

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
        QHash<QString, QStringList>                        groups;

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

            QHashIterator<QString, QStringList> it(groups);
            while (it.hasNext()) {
                it.next();
                auto g = doc.createElementNS(QLatin1String("urn:xmpp:jingle:apps:grouping:0"), QLatin1String("group"));
                g.setAttribute(QLatin1String("semantics"), it.key());
                for (auto const &name : it.value()) {
                    auto c = doc.createElement(QLatin1String("content"));
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
            auto xml = jingle.toXml(&doc);

            for (const QDomElement &e : update) {
                xml.appendChild(e);
            }
            if (needNotifyGroup
                && (action == Action::SessionInitiate || action == Action::SessionAccept || action == Action::ContentAdd
                    || action == Action::ContentAccept)) {
                const auto xmls = genGroupingXML();
                for (auto const &g : xmls)
                    xml.appendChild(g);
                needNotifyGroup = false;
            }

            auto jt = new JT(manager->client()->rootTask());
            jt->request(otherParty, xml);
            QObject::connect(jt, &JT::finished, q, [jt, jingle, callback, this]() {
                waitingAck = false;
                if (callback) {
                    callback(jt);
                }
                if (!jt->success()) {
                    lastError = jt->error();
                }
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
                    for (const auto &h : acceptApps) {
                        auto app      = std::get<0>(h);
                        auto callback = std::get<1>(h);
                        if (app) {
                            callback(jt);
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
            typedef std::tuple<QPointer<Application>, OutgoingUpdateCB> AckHndl;
            if (role == Origin::Responder) {
                for (const auto &c : std::as_const(initialIncomingUnacceptedContent)) {
                    auto out = c->evaluateOutgoingUpdate();
                    if (out.action == Action::ContentReject) {
                        lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                        XMPP::Stanza::Error::ErrorCond::BadRequest);
                        setSessionFinished();
                        return true;
                    }
                    if (out.action != Action::ContentAccept) {
                        return false; // keep waiting.
                    }
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
                sid          = manager->registerSession(q);
                actionToSend = Action::SessionInitiate;
                finalState   = State::Pending;
            }

            notifyPads<&SessionManagerPad::onSend>();

            QList<QDomElement> contents;
            QList<AckHndl>     acceptApps;
            for (const auto &app : std::as_const(contentList)) {
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
            initialIncomingUnacceptedContent.clear();
            sendJingle(actionToSend, contents, [this, acceptApps, finalState](JT *jt) {
                if (!jt->success()) {
                    qDebug("Session accept/initiate returned iq error");
                    emit q->terminated();
                    return;
                }
                state = finalState;
                for (const auto &h : acceptApps) {
                    auto app      = std::get<0>(h);
                    auto callback = std::get<1>(h);
                    if (app) {
                        callback(jt);
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
                        rejectSet.insert(contentName, std::make_pair(ce, cond));
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

        bool handleIncomingContentRemove(const QDomElement &jingleEl)
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
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            state = State::Connecting;
            if (apps.size()) {
                for (auto app : std::as_const(apps)) {
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
                lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                XMPP::Stanza::Error::ErrorCond::BadRequest);
                return false;
            }

            if (apps.size() && state >= State::Active) {
                for (auto app : std::as_const(apps)) {
                    app->start(); // start accepted app. connection establishing and data transfer are inside
                }
            }
            planStep();

            return true;
        }

        bool handleIncomingTransportReplace(const QDomElement &jingleEl)
        {
            qDebug("handle incoming transport replace");
            QVector<std::tuple<Application *, QSharedPointer<Transport>, QDomElement>> passed;
            QList<QDomElement>                                                         toReject;
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
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                Application *app = contentList.value(ContentKey { cb.name, cb.creator });
                if (!app || (app->creator() == role && app->state() <= State::Unacked)) {
                    qDebug("not existing app or inaporpriate app state");
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::ItemNotFound);
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
                if (old->isLocal() && old->state() == State::Unacked && role == Origin::Initiator) {
                    doTieBreak = true;
                    continue;
                }

                if (!app->transportSelector()->canReplace(app->transport(), transport)) {
                    qDebug("incoming unsupported or already used transport");
                    toReject.append(ce);
                    continue;
                }

                if (!app->isTransportReplaceEnabled()) {
                    qDebug("transport replace is disabled for %s", qPrintable(app->contentName()));
                    toReject.append(ce);
                    continue;
                }

                passed.append(std::make_tuple(app, transport, ce));
            }

            for (auto &v : passed) {
                Application              *app;
                QSharedPointer<Transport> transport;
                QDomElement               ce;
                std::tie(app, transport, ce) = v;
                if (doTieBreak) {
                    if (app->transport()->creator() == role && app->transport()->state() < State::Unacked)
                        continue; // it will send transport soon
                    app->selectNextTransport(transport);
                } else if (!app->setTransport(transport)) {
                    // app should generate transport accept eventually. content-accept will
                    // work too if the content wasn't accepted yet
                    toReject.append(ce);
                }
            }

            if (doTieBreak) {
                lastError = ErrorUtil::makeTieBreak(*manager->client()->doc());
                return false;
            } else if (toReject.size()) {
                QList<QDomElement> rejectImported;
                std::transform(toReject.begin(), toReject.end(), std::back_inserter(rejectImported),
                               [this](const QDomElement &e) {
                                   return manager->client()->doc()->importNode(e.cloneNode(true), true).toElement();
                               });
                outgoingUpdates.insert(Action::TransportReject, OutgoingUpdate { rejectImported, OutgoingUpdateCB() });
            }

            planStep();
            return true;
        }

        bool handleIncomingTransportAccept(const QDomElement &jingleEl)
        {
            QString                                    contentTag(QStringLiteral("content"));
            QVector<QPair<Application *, QDomElement>> updates;
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

                Application *app = contentList.value(ContentKey { cb.name, cb.creator });
                if (!app || !app->transport() || app->transport()->creator() != role
                    || app->transport()->state() != State::Pending || transportNS != app->transport()->pad()->ns()) {
                    // ignore out of order
                    qInfo("ignore out of order transport-accept");
                    continue;
                }
                updates.append(qMakePair(app, transportEl));
            }

            for (auto &u : updates) {
                u.first->incomingTransportAccept(u.second);
                // if update fails transport should trigger replace procedure
            }

            planStep();
            return true;
        }

        bool handleIncomingSessionInfo(const QDomElement &jingleEl)
        {
            bool hasElements = false;
            for (QDomElement child = jingleEl.firstChildElement(); !child.isNull();
                 child             = child.nextSiblingElement()) {
                hasElements = true;
                auto pad    = q->applicationPad(child.namespaceURI());
                if (pad) {
                    return pad->incomingSessionInfo(jingleEl); // should return true if supported
                }
            }
            if (!hasElements && state >= State::ApprovedToSend) {
                return true;
            }
            return false;
        }

        bool handleIncomingTransportInfo(const QDomElement &jingleEl)
        {
            QString                                                contentTag(QStringLiteral("content"));
            QVector<QPair<QSharedPointer<Transport>, QDomElement>> updates;
            for (QDomElement ce = jingleEl.firstChildElement(contentTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(contentTag)) {
                Application *app = nullptr;
                ContentBase  cb(ce);
                if (!cb.isValid() || !(app = q->content(cb.name, cb.creator)) || app->state() >= State::Finishing
                    || !app->transport()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                auto tel = ce.firstChildElement(QStringLiteral("transport"));
                if (tel.isNull() || tel.namespaceURI() != app->transport()->pad()->ns()) {
                    lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                                    XMPP::Stanza::Error::ErrorCond::BadRequest);
                    return false;
                }
                updates.append(qMakePair(app->transport(), tel));
            }

            for (auto &u : updates) {
                if (!u.first->update(u.second)) {
                    lastError = u.first->lastError();
                    return false; // failure should trigger transport replace
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

    Features Session::peerFeatures() const { return d->manager->client()->capsManager()->disco(peer()).features(); }

    bool Session::checkPeerCaps(const QString &ns) const
    {
        return d->manager->client()->capsManager()->disco(peer()).features().test(QStringList() << ns);
    }

    bool Session::isGroupingAllowed() const { return d->groupingAllowed; }

    std::optional<XMPP::Stanza::Error> Session::lastError() const { return d->lastError; }

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
        d->groups.insert(groupType, group);
        d->needNotifyGroup = true;
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
        for (auto &c : d->contentList) {
            c->prepare();
        }
        d->notifyPads<&SessionManagerPad::onLocalAccepted>();
        d->planStep();
    }

    void Session::initiate()
    {
        emit initiated();
        if (d->role == Origin::Initiator && d->state == State::Created) {
            d->state = State::ApprovedToSend;
            for (auto &c : d->contentList) {
                c->markInitialApplication(true);
                c->prepare();
            }
            d->notifyPads<&SessionManagerPad::onLocalAccepted>();
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
            Q_ASSERT(!apps.isEmpty());
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

    bool Session::updateFromXml(Action action, const QDomElement &jingleEl)
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
            break;
        case Action::TransportReplace:
            return d->handleIncomingTransportReplace(jingleEl);
        case Action::NoAction:
            break;
        }

        d->lastError = XMPP::Stanza::Error(XMPP::Stanza::Error::ErrorType::Cancel,
                                           XMPP::Stanza::Error::ErrorCond::FeatureNotImplemented);
        return false;
    }
}}

#include "jingle-session.moc"
