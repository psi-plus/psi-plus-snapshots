/*
 * jingle-tiebreaker.h - Session-scoped Jingle tie-break coordination
 * Copyright (C) 2026  Sergei Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef JINGLE_TIEBREAKER_H
#define JINGLE_TIEBREAKER_H

#include <iris/xmpp-im/jingle.h>

#include <QDomDocument>

#include <memory>
#include <optional>

namespace XMPP { namespace Jingle {

    /**
     * Session-scoped coordinator for simultaneous Jingle actions.
     *
     * The coordinator owns only IQ transaction lifetime and resolver dispatch.
     * Application, transport and pad implementations register action-specific
     * resolvers and keep all semantic conflict/retry policy in those owners.
     */
    class IRIS_EXPORT TieBreaker {
        struct SharedState;

    public:
        enum class Solution {
            Continue, ///< Tie-break does not intervene; process the incoming IQ normally.
            Break,    ///< Reject the whole incoming IQ with conflict/tie-break.
            Postpone  ///< Process normally, but preserve local intent if our IQ later fails.
        };

        enum class RemoteResult { Applied, Rejected };

        // retry() is recovery context, not an instruction to replay localData.
        // Resolvers should reconcile the owner's current live intent instead.
        struct RetryContext {
            const QDomElement   &localData;
            const QDomElement   &remoteData;
            const Stanza::Error &localError;
            RemoteResult         remoteResult;
        };

        class Resolver {
        public:
            virtual ~Resolver() = default;
            virtual Solution resolve(const QDomElement &localData, const QDomElement &remoteData)
            {
                Q_UNUSED(localData);
                Q_UNUSED(remoteData);
                return Solution::Continue;
            }
            virtual void retry(const RetryContext &context) { Q_UNUSED(context); }
        };

        class Registration {
            friend class TieBreaker;

        public:
            Registration() = default;
            ~Registration();
            Registration(Registration &&other) noexcept;
            Registration &operator=(Registration &&other) noexcept;
            Registration(const Registration &)            = delete;
            Registration &operator=(const Registration &) = delete;

            explicit operator bool() const { return id_ != 0; }
            bool     isPostponed() const;

        private:
            Registration(std::weak_ptr<SharedState> state, quint64 id);
            void reset();

            std::weak_ptr<SharedState> state_;
            quint64                    id_ = 0;
        };

        struct Resolution {
            Solution solution = Solution::Continue;
            quint64  id       = 0;
            // Resource exhaustion is not a protocol tie-break. The dispatcher
            // must send this error before attempting ordinary action handling.
            std::optional<Stanza::Error> error;
            // For Break, a detached snapshot of the outgoing action that won
            // arbitration. Keep the owner document beside the public element:
            // QDomElement alone does not own the underlying DOM tree.
            QDomDocument localDocument;
            QDomElement  localData;
        };

        TieBreaker();
        ~TieBreaker();
        TieBreaker(const TieBreaker &)            = delete;
        TieBreaker &operator=(const TieBreaker &) = delete;

        Registration registerResolver(Action action, Resolver *resolver);

        // One Session currently serializes outgoing Jingle IQs, but explicit
        // transaction ids keep postponed work tied to the IQ that caused it.
        quint64 outgoingStarted(Action action, const QDomElement &localData);
        void    outgoingFinished(quint64 transaction, const std::optional<Stanza::Error> &error);
        void    outgoingCallbacksFinished(quint64 transaction);

        // Every resolver registered for the matching Action is evaluated. The
        // aggregate wire decision is Break > Postpone > Continue; Break does not
        // short-circuit owner callbacks. Network/selector side effects belong
        // after validation and the incoming reply, not inside resolve().
        Resolution resolveIncoming(Action action, const QDomElement &remoteData);
        // Call only after the incoming IQ reply has been dispatched. May retry
        // synchronously and destroy the coordinator/Session via resolver code.
        void incomingFinished(quint64 resolution, RemoteResult result);

        // Cancel transient arbitration state. Registrations stay valid until
        // their RAII handles are destroyed with their owning session objects.
        void clear();

    private:
        std::shared_ptr<SharedState> state_;
    };

}}

#endif // JINGLE_TIEBREAKER_H
