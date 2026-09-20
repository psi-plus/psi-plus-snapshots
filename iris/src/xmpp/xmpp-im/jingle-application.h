/*
 * jignle-application.h - Base Jingle application classes
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

#ifndef JINGLE_APPLICATION_H
#define JINGLE_APPLICATION_H

#include <iris/iris_export.h>

#include <QMetaObject>
#include <iris/xmpp-im/jingle-tiebreaker.h>
#include <iris/xmpp-im/jingle-transport.h>
#include <optional>

class QTimer;

namespace XMPP { namespace Jingle {

    class ApplicationManager;
    class ApplicationManagerPad : public SessionManagerPad {
        Q_OBJECT
    public:
        typedef QSharedPointer<ApplicationManagerPad> Ptr;

        using SessionManagerPad::SessionManagerPad;

        virtual ApplicationManager *manager() const = 0;

        /*
         * for example we transfer a file
         * then first file may generate name "file1", next "file2" etc
         * As result it will be sent as <content name="file1" ... >
         */
        virtual QString generateContentName(Origin senders) = 0;

        virtual bool incomingSessionInfo(const QDomElement &el);
        // Auxiliary info namespaces need not equal the application description
        // namespace. They must route to the existing pad, not create another one.
        virtual QStringList sessionInfoNamespaces() const { return { ns() }; }
    };

    // Represents a session for single application. for example a single file in a file transfer session.
    // There maybe multiple application instances in a session.
    // It's designed as QObject to exposed to JavaScript (qml/webkit)
    class IRIS_EXPORT Application : public QObject {
        Q_OBJECT
    public:
        struct Update {
            Action action;
            Reason reason;
        };

        enum SetDescError {
            Ok,
            Unparsed,
            IncompatibleParameters // this one is for <reason>
        };

        enum ApplicationFlag {
            InitialApplication = 0x1, // the app came with session-initiate
            UserFlag           = 0x100
        };
        Q_DECLARE_FLAGS(ApplicationFlags, ApplicationFlag)

        ~Application() override;

        virtual void setState(State state) = 0; // likely just remember the state and not generate any signals
        virtual const std::optional<XMPP::Stanza::Error> &lastError() const  = 0;
        virtual Reason                                    lastReason() const = 0;

        inline ApplicationManagerPad::Ptr pad() const { return _pad; }
        inline State                      state() const { return _state; }
        inline Origin                     creator() const { return _creator; }
        inline Origin                     senders() const { return _senders; }
        inline QString                    contentName() const { return _contentName; }
        inline QSharedPointer<Transport>  transport() const { return _transport; }
        inline TransportSelector         *transportSelector() const { return _transportSelector.get(); }
        bool                              isRemote() const;
        inline bool                       isLocal() const { return !isRemote(); }
        inline ApplicationFlags           flags() const { return _flags; }
        inline void                       markInitialApplication(bool state)
        {
            if (state)
                _flags |= InitialApplication;
            else
                _flags &= ~InitialApplication;
        }

        virtual SetDescError setRemoteOffer(const QDomElement &description)  = 0;
        virtual SetDescError setRemoteAnswer(const QDomElement &description) = 0;
        virtual QDomElement  makeLocalOffer()                                = 0;
        virtual QDomElement  makeLocalAnswer()                               = 0;

        /**
         * Process advisory application parameters from description-info.
         * This is not a new offer/answer. Return false for unsupported payloads.
         * Implementations must not run a nested event loop in this callback.
         */
        virtual bool incomingDescriptionInfo(const QDomElement &) { return false; }

        /** Opt in only if the application can enforce changing media direction.
         * This is a capability query, not a consent callback; it must have no side effects.
         * File-transfer applications retain their fixed sending direction by default.
         */
        virtual bool supportsContentModify() const { return false; }

        /** Apply a validated peer direction update. Does not start the application,
         * restart its transport or grant permission to capture local media.
         * Receivers of sendersChanged must enforce local consent independently and
         * must not run a nested event loop during incoming stanza processing.
         */
        void incomingContentModify(Origin senders);

        /** Request a media-direction change.
         * Before the initial content stanza leaves this endpoint, the local proposal
         * is updated synchronously. Afterwards the latest request is queued until the
         * application is active and sent as content-modify. The negotiated direction
         * changes only after the peer acknowledges that request.
         */
        bool requestSenders(Origin senders);

        // Low-level scheduling revision, not a durable policy/operation handle.
        // Returns zero on rejection. A no-op/proposal-only request has no IQ
        // attempt; consumers observe senders() as well as attempt completions.
        quint64 requestSendersTracked(Origin senders);
        // Discard only this queued revision, never an already sent IQ.
        bool cancelQueuedSenders(quint64 revision);
        bool sendersAttemptPending() const { return _sendersUpdateInFlight.has_value(); }

        struct SendersAttemptResult {
            enum class Outcome { Accepted, Rejected, TimedOut, Cancelled };
            quint64                      id       = 0; // unique within this Application incarnation
            quint64                      revision = 0;
            Origin                       target   = Origin::None;
            Outcome                      outcome  = Outcome::Cancelled;
            std::optional<Stanza::Error> error; // includes the IQ timeout error
        };

        /**
         * @brief evaluateOutgoingUpdate computes and prepares next update which will be taken with takeOutgoingUpdate
         *   The updated will be taked immediately if considered to be most preferred among other updates types of
         *   other applications.
         * @return update type
         */
        virtual Update evaluateOutgoingUpdate();
        // this may return something only when evaluateOutgoingUpdate() != NoAction
        virtual OutgoingUpdate takeOutgoingUpdate();

        /**
         * @brief Validate and install a new current transport.
         *
         * Replacement policy is delegated to TransportSelector::replace(). When the
         * application already has a transport, this method also derives the Jingle
         * transport-replace signaling state for the new instance and disconnects the
         * superseded transport. Transport::State is not the lifetime of the
         * transport-replace IQ; see PendingTransportReplace.
         *
         * The transport is retained by QSharedPointer. It is intentionally not made a
         * QObject child of Application, because transport callbacks may outlive one
         * signaling step and can hold shared references of their own.
         *
         * @param transport Candidate transport to make current.
         * @param reason Optional failure reason carried into a subsequent replacement.
         * @return true if the selector accepted and installed the transport.
         */
        bool setTransport(const QSharedPointer<Transport> &transport, const Reason &reason = Reason());

        /**
         * @brief Select the next compatible local transport after failure/replacement.
         *
         * When @p alikeTransport is supplied, it is an advisory peer proposal used by
         * TransportSelector::getAlikeTransport() to choose an efficient compatible local
         * retry. The peer transport itself is not implicitly installed. If no candidate
         * remains, the application moves toward content-remove with failed-transport.
         * Selector calls and emitted signals are reentrant boundaries.
         *
         * @param alikeTransport Optional remote transport used only as a selection hint.
         * @return true if a successor transport was installed.
         */
        bool selectNextTransport(const QSharedPointer<Transport> alikeTransport = QSharedPointer<Transport>());

        /**
         * @brief Return whether this application currently permits transport replacement.
         *
         * Incoming transport-replace validation calls this before mutation. Overrides
         * should behave as a capability/state query and avoid unrelated side effects.
         */
        virtual bool isTransportReplaceEnabled() const;
        virtual bool supportsSharedTransport() const { return false; }

        /**
         * @brief wantBetterTransport checks if the transport is a better match for the application
         * Used in content is provided twice with two different transports
         * @return
         */
        virtual bool wantBetterTransport(const QSharedPointer<Transport> &) const;

        /**
         * @brief prepare to send content-add/session-initiate
         *  When ready, the application first set update type to ContentAdd and then emit updated()
         */
        virtual void prepare()                                                                            = 0;
        virtual void start()                                                                              = 0;
        virtual void remove(Reason::Condition cond = Reason::Success, const QString &comment = QString()) = 0;

        virtual void incomingRemove(const Reason &r) = 0;

        /**
         * @brief Whether this content is awaiting its transport-replace completion callback.
         *
         * This is signaling state (`NeedAck`). Never infer the same fact from
         * Transport::State::Unacked: transport implementations, notably ICE, do not
         * share one transport-state transition for Jingle IQ lifetime. In a batch,
         * the IQ may already be finished while an earlier owner's callback runs.
         * Use the Session TieBreaker, not this flag, for collision arbitration.
         */
        bool transportReplaceAwaitingAck() const;

        // Identity of the current replacement attempt, including same-object
        // reselection. For guarded Session staging; not an IQ lifetime counter.
        quint64 transportReplaceGeneration() const { return _transportReplaceGeneration; }

        /**
         * @brief Whether the current replacement is in the post-IQ negotiation phase.
         *
         * `InProgress` means the replacement proposal is the current signaling attempt
         * known to the peer and is waiting for transport-accept/reject completion.
         */
        bool transportReplaceInProgress() const;

        /**
         * @brief Parse and apply a peer transport-accept to the current replacement.
         *
         * This compatibility overload stages a single payload first. Session batch
         * handling should prepare every sibling before calling the PreparedUpdate overload.
         */
        bool incomingTransportAccept(const QDomElement &el);

        /**
         * @brief Commit an already prepared peer transport-accept payload.
         *
         * Completion is tied to the current transport and replacement generation.
         * commitPreparedUpdate() and start() are reentrant boundaries; a superseding
         * same-pointer generation must not be completed by this acknowledgement.
         */
        bool incomingTransportAccept(Transport::PreparedUpdatePtr update);

        /**
         * @brief Apply a validated peer transport-reject to the current local replacement.
         *
         * A handled rejection returns the signaling state to Planned and asks the
         * TransportSelector for the next local candidate. `true` means the rejection was
         * consumed even when no fallback exists and content removal is scheduled.
         * @return false if the current transaction is not a rejectable local replacement.
         */
        bool incomingTransportReject();

    protected:
        /**
         * @brief wraps transport update so transport can be safely-deleted before callback is triggered
         */
        OutgoingTransportInfoUpdate wrapOutgoingTransportUpdate(bool ensureTransportElement = false);

        /**
         * @brief initTransport in general connects any necessary for the application transport signals
         */
        virtual void prepareTransport() = 0;

        void expectSingleConnection(TransportFeatures features, std::function<void(Connection::Ptr)> &&ready);

    signals:
        void updated(); // signal for session it has to send updates to remote. so it will follow with
                        // takeOutgoingUpdate() eventually
        void stateChanged(State);
        void sendersChanged(Origin);
        // Emitted only when the negotiated direction changes due to a peer content-modify.
        // Local proposals and acknowledgements of our own content-modify do not emit it.
        void sendersChangedByPeer(Origin);
        // One terminal result per consumed content-modify attempt, after internal
        // state cleanup. May be synchronous. QObject::destroyed is the terminal
        // lifetime notification if the Application is deleted before completion.
        void sendersAttemptFinished(const XMPP::Jingle::Application::SendersAttemptResult &result);

    protected:
        State            _state = State::Created;
        ApplicationFlags _flags;

        /**
         * XEP-0166 transport-replace signaling state for this content.
         *
         * This state machine is orthogonal to Transport::State. `NeedAck` means
         * this content's local completion callback has not run yet; the Session's
         * TieBreaker independently tracks whether the batched IQ is outstanding.
         * `InProgress` is the subsequent accept/reject phase (or an incoming peer
         * replacement currently being negotiated).
         */
        enum class PendingTransportReplace {
            None,      ///< No transport-replace signaling transaction is active.
            Planned,   ///< A local successor is selected but not signaled yet.
            NeedAck,   ///< transport-replace was serialized; waiting for this owner's completion.
            InProgress ///< Proposal is current; waiting for transport-accept/reject completion.
        };

        // has to be set when whatever way remote knows about the current transport
        // bool _remoteKnowsOfTheTransport = false;

        // per session object responsible for all applications of this type
        QSharedPointer<ApplicationManagerPad> _pad;

        // content properties as come from the request
        QString _contentName;
        Origin  _creator;
        Origin  _senders;

        // Disposable queued target and concrete IQ target, not durable UI policy.
        std::optional<Origin>               _requestedSenders;
        std::optional<Origin>               _sendersUpdateInFlight;
        QMetaObject::Connection             _sendersStateConnection;
        quint64                             _sendersRequestRevision = 0;
        quint64                             _nextSendersAttempt     = 0;
        std::optional<SendersAttemptResult> _sendersAttempt;

        // Current transport uses shared ownership, independently of QObject parentage.
        // Session handlers pair QPointer<Application> with weak/shared transport snapshots
        // so reentrant callbacks cannot apply stale signaling to a newer transport instance.
        QSharedPointer<Transport>          _transport;
        std::unique_ptr<TransportSelector> _transportSelector;

        // Jingle signaling transaction state for replacing _transport. Do not derive it
        // from Transport::State; concrete transports use those states differently.
        PendingTransportReplace _pendingTransportReplace = PendingTransportReplace::None;
        // Invalidate saved IQ completions on serialization, completion and transport
        // replacement, including reselection of the same Transport object.
        quint64 _transportReplaceGeneration = 0;

        // Reason attached to the pending replacement when the previous transport failed.
        Reason _transportReplaceReason;

        // when set the content will be removed with this reason
        Reason _terminationReason;

        // evaluated update to be sent
        Update _update;

        QTimer *transportInitTimer = nullptr;

    private:
        class ContentModifyTieBreakResolver;
        void ensureContentModifyTieBreakResolver();
        class TransportReplaceTieBreakResolver;
        void ensureTransportReplaceTieBreakResolver();

        // Registration is declared after the resolver so it is destroyed
        // first and never leaves TieBreaker with a dangling callback.
        std::unique_ptr<TieBreaker::Resolver> _contentModifyTieBreakResolver;
        TieBreaker::Registration              _contentModifyTieBreakRegistration;
        std::unique_ptr<TieBreaker::Resolver> _transportReplaceTieBreakResolver;
        TieBreaker::Registration              _transportReplaceTieBreakRegistration;
    };

    inline bool operator<(const Application::Update &a, const Application::Update &b)
    {
        return a.action < b.action || (a.action == b.action && a.reason.condition() < b.reason.condition());
    }

    class ApplicationManager : public QObject {
        Q_OBJECT
    public:
        ApplicationManager(QObject *parent = nullptr);

        virtual void         setJingleManager(Manager *jm) = 0;
        virtual Application *startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName,
                                              Origin creator, Origin senders)
            = 0;
        virtual ApplicationManagerPad *pad(Session *session) = 0;

        // this method is supposed to gracefully close all related sessions as a preparation for plugin unload for
        // example
        virtual void closeAll(const QString &ns = QString()) = 0;

        virtual QStringList ns() const;
        virtual QStringList discoFeatures() const = 0;
    };

}}

Q_DECLARE_METATYPE(XMPP::Jingle::Application::SendersAttemptResult)

#endif
