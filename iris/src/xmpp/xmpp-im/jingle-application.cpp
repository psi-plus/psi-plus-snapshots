/*
 * jignle-application.cpp - Base Jingle application classes
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

#include "jingle-application.h"
#include "jingle-session.h"
#include "xmpp_client.h"
#include "xmpp_task.h"

#include <QPointer>
#include <QTimer>
#include <utility>

namespace XMPP { namespace Jingle {

    static bool isValidSenders(Origin senders)
    {
        return senders == Origin::None || senders == Origin::Both || senders == Origin::Initiator
            || senders == Origin::Responder;
    }

    static QString sendersAttribute(Origin senders)
    {
        switch (senders) {
        case Origin::None:
            return QStringLiteral("none");
        case Origin::Both:
            return QStringLiteral("both");
        case Origin::Initiator:
            return QStringLiteral("initiator");
        case Origin::Responder:
            return QStringLiteral("responder");
        }
        return {};
    }

    class Application::ContentModifyTieBreakResolver : public TieBreaker::Resolver {
    public:
        explicit ContentModifyTieBreakResolver(Application *application) :
            application_(application), key_(application->_contentName, application->_creator)
        {
        }

        TieBreaker::Solution resolve(const QDomElement &localData, const QDomElement &remoteData) override
        {
            if (!contains(localData) || !contains(remoteData))
                return TieBreaker::Solution::Continue;

            const auto application = application_.data();
            if (!application || !application->_pad || !application->_pad->session())
                return TieBreaker::Solution::Continue;

            switch (application->_pad->session()->role()) {
            case Origin::Initiator:
                return TieBreaker::Solution::Break;
            case Origin::Responder:
                return TieBreaker::Solution::Postpone;
            default:
                return TieBreaker::Solution::Continue;
            }
        }

        void retry(const TieBreaker::RetryContext &context) override
        {
            Q_UNUSED(context);
            auto application = application_.data();
            if (!application || application->_state >= State::Finishing || application->_sendersUpdateInFlight
                || !application->_requestedSenders)
                return;

            if (*application->_requestedSenders == application->_senders) {
                application->_requestedSenders.reset();
                return;
            }
            emit application->updated();
        }

    private:
        bool contains(const QDomElement &jingle) const
        {
            for (auto element = jingle.firstChildElement(); !element.isNull(); element = element.nextSiblingElement()) {
                const auto name = element.localName().isEmpty() ? element.tagName() : element.localName();
                if (name != QLatin1String("content"))
                    continue;
                const ContentBase content(element);
                if (content.isValid() && ContentKey { content.name, content.creator } == key_)
                    return true;
            }
            return false;
        }

        QPointer<Application> application_;
        ContentKey            key_;
    };

    class Application::TransportReplaceTieBreakResolver : public TieBreaker::Resolver {
    public:
        explicit TransportReplaceTieBreakResolver(Application *application) : application_(application) { }

        TieBreaker::Solution resolve(const QDomElement &localData, const QDomElement &remoteData) override
        {
            auto app = application_.data();
            if (!app || app->_state >= State::Finishing || app->_pad->session()->role() != Origin::Initiator)
                return TieBreaker::Solution::Continue;
            const ContentKey key { app->_contentName, app->_creator };
            auto             contains = [&key](const QDomElement &data) {
                for (auto el = data.firstChildElement(QStringLiteral("content")); !el.isNull();
                     el      = el.nextSiblingElement(QStringLiteral("content"))) {
                    const ContentBase content(el);
                    if (content.isValid() && ContentKey { content.name, content.creator } == key)
                        return true;
                }
                return false;
            };
            // The coordinator supplies only a genuinely in-flight IQ snapshot.
            // Responder yields through normal processing: replacing the transport
            // invalidates the old completion, while a rejected remote proposal
            // leaves ordinary failed-IQ fallback as the sole recovery owner.
            return contains(localData) && contains(remoteData) ? TieBreaker::Solution::Break
                                                               : TieBreaker::Solution::Continue;
        }

    private:
        QPointer<Application> application_;
    };

    Application::~Application() { emit destroying(); }

    void Application::ensureTransportReplaceTieBreakResolver()
    {
        if (_transportReplaceTieBreakRegistration || !_pad || !_pad->session())
            return;
        _transportReplaceTieBreakResolver = std::make_unique<TransportReplaceTieBreakResolver>(this);
        _transportReplaceTieBreakRegistration
            = _pad->tieBreaker()->registerResolver(Action::TransportReplace, _transportReplaceTieBreakResolver.get());
    }

    void Application::ensureContentModifyTieBreakResolver()
    {
        if (_contentModifyTieBreakRegistration || !_pad || !_pad->session())
            return;
        _contentModifyTieBreakResolver = std::make_unique<ContentModifyTieBreakResolver>(this);
        _contentModifyTieBreakRegistration
            = _pad->tieBreaker()->registerResolver(Action::ContentModify, _contentModifyTieBreakResolver.get());
    }

    void Application::incomingContentModify(Origin senders)
    {
        if (!supportsContentModify() || !isValidSenders(senders) || _senders == senders)
            return;
        _senders = senders;

        QPointer<Application> guard(this);
        emit                  sendersChanged(senders);
        if (!guard)
            return;
        emit sendersChangedByPeer(senders);
        if (!guard)
            return;

        if (_requestedSenders && !_sendersUpdateInFlight) {
            if (*_requestedSenders == _senders)
                _requestedSenders.reset();
            else
                emit updated();
        }
    }

    bool Application::requestSenders(Origin senders) { return requestSendersTracked(senders) != 0; }

    quint64 Application::requestSendersTracked(Origin senders)
    {
        if (!supportsContentModify() || !isValidSenders(senders) || _state >= State::Finishing)
            return 0;

        const auto revision = ++_sendersRequestRevision;

        if (!_sendersStateConnection) {
            qRegisterMetaType<SendersAttemptResult>();
            _sendersStateConnection = connect(this, &Application::stateChanged, this, [this](State state) {
                if (state >= State::Finishing) {
                    _requestedSenders.reset();
                    _sendersUpdateInFlight.reset();
                    const auto attempt = std::exchange(_sendersAttempt, std::nullopt);
                    if (attempt)
                        emit sendersAttemptFinished(*attempt); // default outcome: Cancelled
                    return;
                }
                if (state == State::Active && _requestedSenders && !_sendersUpdateInFlight)
                    emit updated();
            });
        }

        // Before the initial content stanza is consumed by takeOutgoingUpdate(),
        // changing direction only changes the local proposal/answer. No
        // content-modify is needed yet.
        if (_state <= State::ApprovedToSend && !_sendersUpdateInFlight) {
            _requestedSenders.reset();
            if (_senders != senders) {
                _senders = senders;
                emit sendersChanged(senders);
            }
            return revision;
        }

        // If another direction change is already in flight, asking for the
        // currently negotiated value is still meaningful: it supersedes the
        // in-flight request once its IQ result arrives.
        if (!_sendersUpdateInFlight && _senders == senders) {
            _requestedSenders.reset();
            return revision;
        }
        if (_requestedSenders && *_requestedSenders == senders)
            return revision;

        _requestedSenders = senders;
        emit updated();
        return revision;
    }

    bool Application::cancelQueuedSenders(quint64 revision)
    {
        if (!revision || revision != _sendersRequestRevision)
            return false;
        _requestedSenders.reset();
        return true;
    }

    class ConnectionWaiter : public QObject {
        Q_OBJECT

        std::function<void(Connection::Ptr)> ready;
        std::function<void()>                failed;
        Connection::Ptr                      connection;
        QWeakPointer<Transport>              transport;

        void waitConnected()
        {
            connect(connection.data(), &Connection::error, this,
                    [this](int code) { onFailed(QString("error=%1").arg(code)); });
            connect(connection.data(), &Connection::connected, this, &ConnectionWaiter::onReady);
        }

        void onFailed(const QString &errorMessage = QString())
        {
            if (!errorMessage.isEmpty())
                qDebug("ConnectionWaiter: error: %s", qPrintable(errorMessage));
            if (connection)
                connection->disconnect(this); // qt signals
            if (auto t = transport.lock()) {
                t->disconnect(this);
            }
            failed();
            deleteLater();
        }

        void onReady()
        {
            connection->disconnect(this); // qt signals
            transport.lock()->disconnect(this);
            ready(connection);
            deleteLater();
        }

    public:
        ConnectionWaiter(TransportFeatures features, std::function<void(Connection::Ptr)> &&ready,
                         std::function<void()> &&failed, Application *app) :
            QObject(app), ready(std::move(ready)), failed(std::move(failed))
        {
            auto tr   = app->transport();
            transport = tr;
            Q_ASSERT(!tr.isNull());

            connect(tr.data(), &Transport::stateChanged, this, [this]() {
                auto locked = transport.lock();
                if (locked->state() == State::Finished) {
                    onFailed(QLatin1String("Transport") + locked->pad()->ns()
                             + QLatin1String(" is in finished state and no connection => transport failure"));
                }
            });
            if (tr->isLocal()) {
                connection = tr->addChannel(features, app->contentName());
                if (!connection) {
                    onFailed(QString("No channel on %1 transport").arg(tr->pad()->ns()));
                    return;
                }
                waitConnected();
            } else {
                app->transport()->addAcceptor(
                    features, [this, self = QPointer<ConnectionWaiter>(this)](Connection::Ptr newConnection) {
                        if (!self || connection)
                            return false;
                        connection = newConnection;
                        if (connection->isOpen())
                            onReady();
                        else
                            waitConnected();
                        return true;
                    });
            }
        }

        ~ConnectionWaiter() { qDebug("~ConnectionWaiter"); }
    };

    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    ApplicationManager::ApplicationManager(QObject *parent) : QObject(parent) { }
    QStringList ApplicationManager::ns() const { return discoFeatures(); }

    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    Application::Update Application::evaluateOutgoingUpdate()
    {
        _update = { Action::NoAction, Reason() };
        if (_state == State::Finished || _state == State::Created
            || _pendingTransportReplace == PendingTransportReplace::NeedAck)
            return _update;

        if (_terminationReason.isValid()) {
            _update = { Action::ContentRemove, _terminationReason };
            return _update;
        }

        // missing transport means it's an incoming application with invalid transport,
        // but basically it shouldn't happen
        if ((isRemote() && _state == State::Pending) || !_transport) {
            return _update;
        }

        bool inTrReplace = _pendingTransportReplace == PendingTransportReplace::InProgress;

        if (_transport->state() == State::Finished) {
            if (inTrReplace && _transport->creator() != _pad->session()->role())
                _update = { Action::TransportReject, _transport->lastReason() };
            else
                _update = { _transportSelector->hasMoreTransports() ? Action::TransportReplace : Action::ContentRemove,
                            _transport->lastReason() };
            return _update;
        }

        switch (_state) {
        case State::ApprovedToSend:
            if (_transport->state() >= State::Accepted) {
                _update
                    = { _pad->session()->role() == _creator ? Action::ContentAdd : Action::ContentAccept, Reason() };
            } else if (_transport->hasUpdates() && _transport->state() == State::ApprovedToSend) {
                if (_pendingTransportReplace == PendingTransportReplace::Planned) {
                    _update = { Action::TransportReplace, _transportReplaceReason };
                } else if (inTrReplace) { // both sides already know it's replace. but not accepted yet.
                    _update = { _transport->isLocal() ? Action::TransportInfo : Action::TransportAccept, Reason() };
                } else
                    _update = { _pad->session()->role() == _creator ? Action::ContentAdd : Action::ContentAccept,
                                Reason() };
            }
            break;
        case State::Pending:
            if (_creator != _pad->session()->role() && !inTrReplace && _transport->hasUpdates()
                && _transport->state() == State::ApprovedToSend) {
                // if remote transport has initial updates and it's not transport-replace then it's time to accept the
                // content
                _update = { Action::ContentAccept, Reason() };
                break;
            }
            // fallthrough
        case State::Connecting:
            if (inTrReplace) {
                // for transport replace we handle just replace until it's finished
                if (_transport->state() == State::Finished) { // 2nd transport failed shortly
                    _update = { _transportSelector->hasMoreTransports()
                                    ? (_transport->isLocal() ? Action::TransportReplace : Action::TransportReject)
                                    : Action::ContentRemove,
                                _transport->lastReason() };
                } else if (_transport->hasUpdates() && _transport->state() == State::ApprovedToSend) {
                    _update = { _transport->isLocal() ? Action::TransportInfo : Action::TransportAccept, Reason() };
                }
                break;
            }

            if (_transport->hasUpdates()) {
                if (_transport->state() >= State::ApprovedToSend && _transport->state() < State::Finished)
                    _update = { _pendingTransportReplace == PendingTransportReplace::Planned ? Action::TransportReplace
                                                                                             : Action::TransportInfo,
                                Reason() };
            } else if (_transport->state() == State::Finished) {
                _update = { _transportSelector->hasMoreTransports() ? Action::TransportReplace : Action::ContentRemove,
                            _transport->lastReason() };
            }
            break;
        case State::Active:
            // Preserve the action priority defined by Action: flush transport
            // updates before changing media direction.
            if (_transport->hasUpdates()) {
                _update = { Action::TransportInfo, Reason() };
            } else if (_requestedSenders && !_sendersUpdateInFlight) {
                if (*_requestedSenders == _senders)
                    _requestedSenders.reset();
                else
                    _update = { Action::ContentModify, Reason() };
            }
            break;
        default:
            break;
        }
        return _update;
    }

    OutgoingUpdate Application::takeOutgoingUpdate()
    {

        QDomElement      transportEl;
        OutgoingUpdateCB transportCB;
        auto             client = _pad->session()->manager()->client();
        auto             doc    = client->doc();

        ContentBase cb(_creator, _contentName);
        // we need to send senders for initial offer/answer
        if (_state == State::ApprovedToSend)
            cb.senders = _senders;
        QList<QDomElement> updates;
        auto               contentEl = cb.toXml(doc, "content");
        updates << contentEl;

        switch (_update.action) {
        case Action::ContentReject:
        case Action::ContentRemove:
            if (_update.reason.isValid())
                updates << _update.reason.toXml(doc);
            return OutgoingUpdate { updates, [this](bool) { setState(State::Finished); } };
        case Action::ContentAdd:
            contentEl.appendChild(makeLocalOffer());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);

            setState(State::Unacked);
            return OutgoingUpdate { updates, [this, transportCB](Task *task) {
                                       transportCB(task);
                                       if (task->success())
                                           setState(State::Pending);
                                   } };

        case Action::ContentAccept:
            contentEl.appendChild(makeLocalAnswer());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate(true);
            contentEl.appendChild(transportEl);

            setState(State::Unacked);
            return OutgoingUpdate { updates, [this, transportCB](Task *task) {
                                       transportCB(task);
                                       if (task->success())
                                           setState(State::Connecting);
                                   } };
        case Action::ContentModify: {
            Q_ASSERT(_state == State::Active);
            Q_ASSERT(_requestedSenders);
            Q_ASSERT(!_sendersUpdateInFlight);
            const auto requested = *_requestedSenders;
            ensureContentModifyTieBreakResolver();

            // XEP-0166 makes senders mandatory for content-modify. ContentBase
            // normally omits the default value "both", so force the attribute
            // for every direction here.
            contentEl.setAttribute(QLatin1String("senders"), sendersAttribute(requested));
            _sendersUpdateInFlight = requested;
            const auto attemptId   = ++_nextSendersAttempt;
            const auto revision    = _sendersRequestRevision;
            _sendersAttempt        = SendersAttemptResult { attemptId, revision, requested };
            return OutgoingUpdate { updates,
                                    [this, guard = QPointer<Application>(this), requested, attemptId,
                                     revision](Task *task) {
                                        if (!guard || !_sendersAttempt || _sendersAttempt->id != attemptId)
                                            return; // deletion, cancellation or duplicate/stale completion
                                        const bool success   = task && task->success();
                                        const bool postponed = _contentModifyTieBreakRegistration.isPostponed();
                                        auto       result    = *_sendersAttempt;
                                        result.outcome       = success ? SendersAttemptResult::Outcome::Accepted
                                                                       : SendersAttemptResult::Outcome::Rejected;
                                        if (!success)
                                            result.error = task ? task->error() : Stanza::Error();
                                        if (task && !success && task->statusCode() == Task::ErrTimeout) {
                                            result.outcome = SendersAttemptResult::Outcome::TimedOut;
                                            result.error   = Stanza::Error(Stanza::Error::ErrorType::Wait,
                                                                           Stanza::Error::ErrorCond::RemoteServerTimeout);
                                        } else if (!task || (!success && task->statusCode() == Task::ErrDisc)) {
                                            result.outcome = SendersAttemptResult::Outcome::Cancelled;
                                        }
                                        _sendersAttempt.reset();
                                        _sendersUpdateInFlight.reset();
                                        const bool changed = success && _senders != requested;
                                        if (success)
                                            _senders = requested;

                                        // A generic failure drops an unchanged request as before. A
                                        // postponed crossed action keeps its local intent until TieBreaker
                                        // calls retry() after this owner callback has fully completed.
                                        if (!success && !postponed && _sendersRequestRevision == revision)
                                            _requestedSenders.reset();
                                        if (_requestedSenders && *_requestedSenders == _senders)
                                            _requestedSenders.reset();

                                        // Publish only after cleanup: callbacks may enqueue a newer
                                        // revision, finish the content or delete it synchronously.
                                        if (changed)
                                            emit sendersChanged(requested);
                                        if (!guard)
                                            return;
                                        emit sendersAttemptFinished(result);
                                        if (!guard || _state >= State::Finishing)
                                            return;
                                        if (_requestedSenders && !postponed)
                                            emit updated();
                                    } };
        }
        case Action::TransportInfo:
            Q_ASSERT(_transport->hasUpdates());
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            return OutgoingUpdate { updates, transportCB };
        // transport-replace IQ lifetime belongs to PendingTransportReplace, not
        // Transport::State. Capture the concrete transport as transaction identity:
        // its callback is a reentrant boundary and may install a newer replacement.
        case Action::TransportReplace: {
            ensureTransportReplaceTieBreakResolver();
            Q_ASSERT(_transport->hasUpdates());
            const auto replacement             = _transport.toWeakRef();
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            if (_pendingTransportReplace == PendingTransportReplace::Planned)
                _pendingTransportReplace = PendingTransportReplace::NeedAck;
            const auto generation = ++_transportReplaceGeneration;
            if (_update.reason.isValid())
                updates << _update.reason.toXml(doc);
            return OutgoingUpdate {
                updates,
                [this, guard = QPointer<Application>(this), replacement, transportCB, generation](Task *task) {
                    if (!guard || _state >= State::Finishing || _transportReplaceGeneration != generation)
                        return;
                    auto expected = replacement.lock();
                    if (!expected || _transport != expected
                        || _pendingTransportReplace != PendingTransportReplace::NeedAck)
                        return;

                    // Retire the IQ before invoking any transport-specific code.
                    // A callback may deliver another incoming IQ, duplicate this
                    // completion, delete us or install a newer replacement.
                    const bool accepted            = task && task->success();
                    const auto completedGeneration = ++_transportReplaceGeneration;
                    _pendingTransportReplace
                        = accepted ? PendingTransportReplace::InProgress : PendingTransportReplace::Planned;
                    transportCB(task);
                    if (!guard || accepted || _state >= State::Finishing
                        || _transportReplaceGeneration != completedGeneration || _transport != expected
                        || _pendingTransportReplace != PendingTransportReplace::Planned)
                        return;

                    // The peer did not acknowledge this replacement. Do not leave
                    // the application blocked in NeedAck; move to the next local
                    // candidate (or content-remove if none remain).
                    selectNextTransport();
                }
            };
        }
        // This ACK completes a peer-initiated replacement. Treat the outgoing
        // transport-accept as its own attempt: validate and retire that attempt before
        // invoking transport-specific code. The callback is a reentrant boundary and
        // may delete us, install even the same Transport as a new generation, or cause
        // this completion to be delivered again.
        case Action::TransportAccept: {
            Q_ASSERT(_transport->hasUpdates());
            const auto accepted                = _transport.toWeakRef();
            const auto generation              = _transportReplaceGeneration;
            std::tie(transportEl, transportCB) = wrapOutgoingTransportUpdate();
            contentEl.appendChild(transportEl);
            return OutgoingUpdate {
                updates,
                [this, guard = QPointer<Application>(this), accepted, transportCB, generation](Task *task) {
                    if (!guard || _state >= State::Finishing || _transportReplaceGeneration != generation)
                        return;
                    auto expected = accepted.lock();
                    if (!expected || _transport != expected
                        || _pendingTransportReplace != PendingTransportReplace::InProgress)
                        return;

                    const bool succeeded         = task && task->success();
                    const auto retiredGeneration = ++_transportReplaceGeneration;
                    if (succeeded)
                        _pendingTransportReplace = PendingTransportReplace::None;

                    transportCB(task);
                    if (!guard || _state >= State::Finishing || _transportReplaceGeneration != retiredGeneration
                        || _transport != expected)
                        return;

                    if (succeeded) {
                        if (_pendingTransportReplace != PendingTransportReplace::None)
                            return;
                        if (_state == State::Connecting || _state == State::Active)
                            expected->start();
                    }
                    // On failure the transport callback retains ownership of
                    // transport-specific failure/fallback handling. The generation
                    // retirement above still makes duplicate/stale completions inert.
                }
            };
        }
        default:
            break;
        }

        return OutgoingUpdate(); // TODO
    }

    OutgoingTransportInfoUpdate Application::wrapOutgoingTransportUpdate(bool ensureTransportElement)
    {
        QDomElement      transportEl;
        OutgoingUpdateCB transportCB;
        std::tie(transportEl, transportCB) = _transport->takeOutgoingUpdate(ensureTransportElement);
        auto wrapCB                        = [tr = _transport.toWeakRef(), cb = std::move(transportCB)](Task *task) {
            auto transport = tr.lock();
            if (!transport) {
                return;
            }
            if (cb)
                cb(task);
        };
        return OutgoingTransportInfoUpdate { transportEl, wrapCB };
    }

    void Application::expectSingleConnection(TransportFeatures features, std::function<void(Connection::Ptr)> &&ready)
    {
        const auto expected = _transport.toWeakRef();
        new ConnectionWaiter(
            features, std::move(ready),
            [this, expected]() {
                auto failedTransport = expected.lock();
                if (!failedTransport || _transport != failedTransport)
                    return;
                qDebug("Application::expectSingleConnection: stopping failed %s transport",
                       qPrintable(failedTransport->pad()->ns()));
                failedTransport->stop();
                selectNextTransport();
            },
            this);
    }

    bool Application::isRemote() const { return _pad->session()->role() != _creator; }

    bool Application::selectNextTransport(const QSharedPointer<Transport> alikeTransport)
    {
        qDebug("selecting next transport");
        const auto expected   = _transport;
        const auto generation = _transportReplaceGeneration;
        auto       current    = [this, guard = QPointer<Application>(this), expected, generation] {
            return guard && _state < State::Finishing && _transport == expected
                && _transportReplaceGeneration == generation;
        };
        auto failNoTransport = [this, &current]() {
            if (!current())
                return false;
            if (_transport) {
                qDebug("Application::selectNextTransport: stopping %s transport", qPrintable(_transport->pad()->ns()));
                _transport->disconnect(this);
                _transport->stop();
                if (!current())
                    return false;
            }
            _state             = (isRemote() || _state > State::ApprovedToSend) ? State::Finishing : State::Finished;
            _terminationReason = Reason(Reason::FailedTransport);
            emit updated(); // will be evaluated to content-remove
            return false;
        };

        if (!current())
            return false;

        // If the application policy cannot migrate the current transport,
        // do not enter a selector loop that may keep returning the same
        // unconsumed candidate forever. RTP deliberately allows this while an
        // unbundled call is still Connecting, but not for established media or
        // an unsupported shared-transport migration.
        if (_transport && !isTransportReplaceEnabled())
            return failNoTransport();

        const bool hasMore = _transportSelector->hasMoreTransports();
        if (!current())
            return false;
        if (!hasMore)
            return failNoTransport();

        if (alikeTransport) {
            auto tr = _transportSelector->getAlikeTransport(alikeTransport);
            if (!current())
                return false;
            if (tr && setTransport(tr))
                return true;
            if (!current())
                return false;
        }

        while (current()) {
            auto t = _transportSelector->getNextTransport();
            if (!current())
                return false;
            if (!t)
                break;
            if (setTransport(t))
                return true;
        }

        if (!current())
            return false;
        if (!_transportSelector->hasMoreTransports())
            return failNoTransport();

        emit updated(); // selector may have a transiently unavailable candidate
        return false;
    }

    bool Application::wantBetterTransport(const QSharedPointer<Transport> &t) const
    {
        if (!_transportSelector->hasTransport(t))
            return false;

        return !_transport || _transportSelector->compare(t, _transport) > 0;
    }

    bool Application::transportReplaceAwaitingAck() const
    {
        return _pendingTransportReplace == PendingTransportReplace::NeedAck;
    }

    bool Application::transportReplaceInProgress() const
    {
        return _pendingTransportReplace == PendingTransportReplace::InProgress;
    }

    bool Application::incomingTransportAccept(const QDomElement &el)
    {
        if (_pendingTransportReplace != PendingTransportReplace::InProgress || !_transport)
            return false;

        auto prepared = _transport->prepareUpdate(el);
        if (!prepared)
            return false;
        return incomingTransportAccept(std::move(prepared.update));
    }

    bool Application::incomingTransportAccept(Transport::PreparedUpdatePtr update)
    {
        if (_pendingTransportReplace != PendingTransportReplace::InProgress || !_transport || !update)
            return false;

        const auto            expected   = _transport;
        const auto            generation = _transportReplaceGeneration;
        QPointer<Application> guard(this);
        if (!expected->commitPreparedUpdate(std::move(update)))
            return false;
        if (!guard)
            return true;

        // Committing a transport payload may reenter application code and even
        // reselect the same Transport object. Complete only the exact attempt
        // for which this prepared value was staged.
        if (_state >= State::Finishing || _transport != expected || _transportReplaceGeneration != generation
            || _pendingTransportReplace != PendingTransportReplace::InProgress)
            return true;

        ++_transportReplaceGeneration;
        _pendingTransportReplace = PendingTransportReplace::None;
        if (_state >= State::Connecting)
            expected->start();
        return true;
    }
    bool Application::incomingTransportReject()
    {
        if (_pendingTransportReplace != PendingTransportReplace::InProgress || !_transport || !_transport->isLocal())
            return false;

        // The peer rejected the current proposal. Any selected successor is a
        // fresh proposal and must be signalled with another transport-replace.
        _pendingTransportReplace = PendingTransportReplace::Planned;
        selectNextTransport();
        return true;
    }

    bool Application::isTransportReplaceEnabled() const { return true; }

    bool Application::setTransport(const QSharedPointer<Transport> &transport, const Reason &reason)
    {
        // Pin both transports (the argument may alias our member) and check
        // identity after selector callbacks, including same-object reselection.
        const auto replacement       = transport;
        const auto expected          = _transport;
        const auto generation        = _transportReplaceGeneration;
        const auto replacementReason = reason;
        auto       current           = [this, guard = QPointer<Application>(this), expected, generation] {
            return guard && _state < State::Finishing && _transport == expected
                && _transportReplaceGeneration == generation;
        };
        if (!replacement || !current())
            return false;
        const bool enabled = isTransportReplaceEnabled();
        if (!current() || !enabled)
            return false;
        const bool accepted = _transportSelector->replace(expected, replacement);
        if (!current() || !accepted)
            return false;

        if (expected && expected->state() < State::Unacked && expected->creator() == _pad->session()->role()
            && expected->pad()->ns() != replacement->pad()->ns()) {
            _transportSelector->backupTransport(expected);
            if (!current())
                return false;
        }
        ++_transportReplaceGeneration;

        // in case we automatically select a new transport on our own we definitely will come up to this point
        if (_transport) {
            if (replacement->isLocal()) {
                auto ts = _transport->state() == State::Finished ? _transport->prevState() : _transport->state();
                if (_transport->isRemote() || ts >= State::Unacked) {
                    // Even when the previous transport still calls itself Unacked,
                    // this successor has not been sent. It cannot inherit an IQ
                    // completion belonging to the previous transport instance.
                    _pendingTransportReplace = PendingTransportReplace::Planned;
                }
            } else {
                _pendingTransportReplace = PendingTransportReplace::InProgress;
            }

            if (_pendingTransportReplace != PendingTransportReplace::None) {
                if (_transport->state() == State::Finished) { // initiate replace?
                    _transportReplaceReason
                        = replacementReason.isValid() ? replacementReason : _transport->lastReason();
                } else {
                    _transportReplaceReason = replacementReason;
                }
            }
            qDebug("Application::setTransport: resetting %s transport in favor of %s",
                   qPrintable(_transport->pad()->ns()), qPrintable(replacement->pad()->ns()));
            _transport->disconnect(this);
            _transport.reset();
        } else {
            qDebug("setting transport %s", qPrintable(replacement->pad()->ns()));
        }

        _transport = replacement;

        if (auto session = _pad ? _pad->session() : nullptr)
            session->refreshAutomaticGroupings();

        connect(_transport.data(), &Transport::updated, this, &Application::updated);
        connect(_transport.data(), &Transport::failed, this, [this]() { selectNextTransport(); });

        if (_transport && _transport->state() < State::Finishing && _state >= State::ApprovedToSend) {
            QTimer::singleShot(0, this, [this, wp = _transport.toWeakRef()]() {
                auto p = wp.lock();
                if (p && p == _transport) {
                    prepareTransport();
                }
            });
        }

        return true;
    }

    bool ApplicationManagerPad::incomingSessionInfo(const QDomElement &) { return false; /* unsupported by default */ }

}}

#include "jingle-application.moc"
