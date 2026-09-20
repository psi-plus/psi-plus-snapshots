/*
 * jingle-tiebreaker.cpp - Session-scoped Jingle tie-break coordination
 * Copyright (C) 2026  Sergei Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "jingle-tiebreaker.h"

#include <QDomElement>
#include <QHash>
#include <QMap>

namespace XMPP { namespace Jingle {

    namespace {
        // Bound only arbitration snapshots, not the general stanza parser.
        // Walk iteratively before cloning so deeply nested XML cannot cause an
        // unbounded recursive copy or peer-controlled retained collision history.
        bool boundedSnapshot(const QDomElement &root)
        {
            constexpr int maxNodes = 4096, maxDepth = 64, maxCharacters = 256 * 1024;
            int           nodes = 0, depth = 0;
            qsizetype     characters = 0;
            QDomNode      node       = root;
            while (!node.isNull()) {
                if (++nodes > maxNodes || depth > maxDepth)
                    return false;
                characters += node.nodeName().size() + node.nodeValue().size();
                const auto attributes = node.attributes();
                for (int i = 0; i < attributes.count(); ++i) {
                    if (++nodes > maxNodes)
                        return false;
                    const auto attribute = attributes.item(i);
                    characters += attribute.nodeName().size() + attribute.nodeValue().size();
                    if (characters > maxCharacters)
                        return false;
                }
                if (characters > maxCharacters)
                    return false;
                if (!node.firstChild().isNull()) {
                    node = node.firstChild();
                    ++depth;
                    continue;
                }
                while (node != root && node.nextSibling().isNull()) {
                    node = node.parentNode();
                    --depth;
                }
                if (node == root)
                    break;
                node = node.nextSibling();
            }
            return true;
        }
    }

    struct TieBreaker::SharedState {
        struct ResolverEntry {
            Action    action    = Action::NoAction;
            Resolver *resolver  = nullptr;
            int       postponed = 0;
        };
        struct Transaction {
            quint64                      id     = 0;
            Action                       action = Action::NoAction;
            QDomDocument                 localDocument;
            QDomElement                  localData;
            bool                         finished          = false;
            bool                         callbacksFinished = false;
            std::optional<Stanza::Error> error;
        };
        struct PendingResolution {
            quint64                     id          = 0;
            quint64                     transaction = 0;
            QDomDocument                remoteDocument;
            QDomElement                 remoteData;
            QList<quint64>              resolvers;
            std::optional<RemoteResult> remoteResult;
        };

        QMap<quint64, ResolverEntry>      resolvers;
        QHash<quint64, Transaction>       transactions;
        QHash<quint64, PendingResolution> resolutions;
        quint64                           currentOutgoing = 0;
        quint64                           nextResolver    = 0;
        quint64                           nextTransaction = 0;
        quint64                           nextResolution  = 0;
        quint64                           epoch           = 0;

        QList<quint64> resolutionsFor(quint64 transaction) const
        {
            QList<quint64> result;
            for (auto it = resolutions.cbegin(); it != resolutions.cend(); ++it) {
                if (it->transaction == transaction)
                    result.append(it.key());
            }
            return result;
        }

        void releaseResolution(quint64 id)
        {
            auto it = resolutions.find(id);
            if (it == resolutions.end())
                return;
            const auto resolverIds = it->resolvers;
            resolutions.erase(it);
            for (auto resolverId : resolverIds) {
                auto resolver = resolvers.find(resolverId);
                if (resolver != resolvers.end() && resolver->postponed > 0)
                    --resolver->postponed;
            }
        }

        void maybeReleaseTransaction(quint64 id)
        {
            auto it = transactions.find(id);
            if (it == transactions.end() || !it->finished || !it->callbacksFinished)
                return;
            if (!resolutionsFor(id).isEmpty())
                return;
            transactions.erase(it);
        }

        void tryRetry(quint64 resolutionId)
        {
            auto resolution = resolutions.find(resolutionId);
            if (resolution == resolutions.end() || !resolution->remoteResult)
                return;
            auto transaction = transactions.find(resolution->transaction);
            if (transaction == transactions.end() || !transaction->finished || !transaction->callbacksFinished
                || !transaction->error)
                return;

            // Copy everything a callback may need before removing the pending
            // resolution. retry() may synchronously unregister resolvers or
            // create another outgoing Jingle action.
            const auto transactionId = transaction->id;
            // Pin both owner documents across state removal and arbitrary callbacks.
            const auto localDocument  = transaction->localDocument;
            const auto localData      = transaction->localData;
            const auto remoteDocument = resolution->remoteDocument;
            const auto remoteData     = resolution->remoteData;
            const auto localError     = *transaction->error;
            const auto remoteResult  = *resolution->remoteResult;
            const auto resolverIds   = resolution->resolvers;
            const auto dispatchEpoch = epoch;
            releaseResolution(resolutionId);
            maybeReleaseTransaction(transactionId);

            const RetryContext context { localData, remoteData, localError, remoteResult };
            for (auto resolverId : resolverIds) {
                if (epoch != dispatchEpoch)
                    break;
                auto entry = resolvers.find(resolverId);
                if (entry != resolvers.end() && entry->resolver)
                    entry->resolver->retry(context);
            }
        }
    };

    TieBreaker::Registration::Registration(std::weak_ptr<SharedState> state, quint64 id) :
        state_(std::move(state)), id_(id)
    {
    }

    TieBreaker::Registration::~Registration() { reset(); }

    TieBreaker::Registration::Registration(Registration &&other) noexcept :
        state_(std::move(other.state_)), id_(other.id_)
    {
        other.id_ = 0;
    }

    TieBreaker::Registration &TieBreaker::Registration::operator=(Registration &&other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        state_    = std::move(other.state_);
        id_       = other.id_;
        other.id_ = 0;
        return *this;
    }

    void TieBreaker::Registration::reset()
    {
        if (!id_)
            return;
        if (auto state = state_.lock())
            state->resolvers.remove(id_);
        id_ = 0;
        state_.reset();
    }

    bool TieBreaker::Registration::isPostponed() const
    {
        if (!id_)
            return false;
        if (auto state = state_.lock()) {
            const auto it = state->resolvers.constFind(id_);
            return it != state->resolvers.cend() && it->postponed > 0;
        }
        return false;
    }

    TieBreaker::TieBreaker() : state_(std::make_shared<SharedState>()) { }
    TieBreaker::~TieBreaker()
    {
        clear(); // Invalidate callback snapshots even while SharedState is pinned.
        state_->resolvers.clear();
    }

    TieBreaker::Registration TieBreaker::registerResolver(Action action, Resolver *resolver)
    {
        if (!resolver || action == Action::NoAction)
            return {};
        auto id = ++state_->nextResolver;
        state_->resolvers.insert(id, SharedState::ResolverEntry { action, resolver, 0 });
        return Registration(state_, id);
    }

    quint64 TieBreaker::outgoingStarted(Action action, const QDomElement &localData)
    {
        // Session serializes outgoing IQs. This is not a multi-IQ scheduler.
        Q_ASSERT(!state_->currentOutgoing);
        const auto id = ++state_->nextTransaction;
        SharedState::Transaction transaction;
        transaction.id            = id;
        transaction.action        = action;
        transaction.localData     = transaction.localDocument.importNode(localData, true).toElement();
        state_->transactions.insert(id, transaction);
        state_->currentOutgoing = id;
        return id;
    }

    void TieBreaker::outgoingFinished(quint64 transactionId, const std::optional<Stanza::Error> &error)
    {
        const auto state       = state_; // Arbitrary callbacks may destroy this coordinator.
        auto       transaction = state->transactions.find(transactionId);
        if (transaction == state->transactions.end() || transaction->finished)
            return;
        if (state->currentOutgoing == transactionId)
            state->currentOutgoing = 0; // clear collision lifetime before owner callbacks
        transaction->finished = true;
        transaction->error    = error;

        if (!error) {
            // The peer accepted our proposal. A postponed local intent therefore
            // needs no tie-break recovery, irrespective of the remote IQ outcome.
            const auto resolutions = state->resolutionsFor(transactionId);
            for (auto resolution : resolutions)
                state->releaseResolution(resolution);
        } else {
            const auto resolutions = state->resolutionsFor(transactionId);
            for (auto resolution : resolutions)
                state->tryRetry(resolution);
        }
        state->maybeReleaseTransaction(transactionId);
    }

    void TieBreaker::outgoingCallbacksFinished(quint64 transactionId)
    {
        const auto state       = state_;
        auto       transaction = state->transactions.find(transactionId);
        if (transaction == state->transactions.end() || transaction->callbacksFinished)
            return;
        transaction->callbacksFinished = true;
        const auto resolutions         = state->resolutionsFor(transactionId);
        for (auto resolution : resolutions)
            state->tryRetry(resolution);
        state->maybeReleaseTransaction(transactionId);
    }

    TieBreaker::Resolution TieBreaker::resolveIncoming(Action action, const QDomElement &remoteData)
    {
        const auto state       = state_;
        auto       transaction = state->transactions.constFind(state->currentOutgoing);
        if (transaction == state->transactions.cend() || transaction->action != action)
            return {};
        if (state->resolutions.size() >= 64 || !boundedSnapshot(remoteData))
            return { Solution::Continue, 0,
                     Stanza::Error(Stanza::Error::ErrorType::Wait, Stanza::Error::ErrorCond::ResourceConstraint) };
        // Never carry a container iterator (or a reference into it) across user code.
        const auto transactionId = transaction->id;
        const auto localData     = transaction->localData;
        QDomDocument remoteDocument;
        const auto remoteSnapshot = remoteDocument.importNode(remoteData, true).toElement();
        if (remoteSnapshot.isNull())
            return {};
        const auto epoch = state->epoch;

        QList<quint64> resolverIds;
        for (auto it = state->resolvers.cbegin(); it != state->resolvers.cend(); ++it) {
            if (it->action == action)
                resolverIds.append(it.key());
        }

        QList<quint64> postponed;
        bool           shouldBreak = false;
        for (auto resolverId : resolverIds) {
            auto entry = state->resolvers.find(resolverId);
            if (entry == state->resolvers.end() || !entry->resolver)
                continue;
            const auto solution = entry->resolver->resolve(localData, remoteSnapshot);
            if (state->epoch != epoch || state->currentOutgoing != transactionId)
                return {};
            if (solution == Solution::Break)
                shouldBreak = true;
            else if (solution == Solution::Postpone && state->resolvers.contains(resolverId))
                postponed.append(resolverId);
        }

        // Resolve every registered owner even when one already requested Break.
        // The aggregate wire decision is deterministic: Break dominates
        // Postpone, and Postpone dominates Continue.
        if (shouldBreak) {
            Resolution result;
            result.solution  = Solution::Break;
            result.localData = result.localDocument.importNode(localData, true).toElement();
            return result;
        }
        if (postponed.isEmpty())
            return {};

        const auto id = ++state->nextResolution;
        SharedState::PendingResolution pending;
        pending.id             = id;
        pending.transaction    = transactionId;
        pending.remoteDocument = remoteDocument;
        pending.remoteData     = remoteSnapshot;
        pending.resolvers      = postponed;
        state->resolutions.insert(id, pending);
        for (auto resolverId : postponed) {
            auto entry = state->resolvers.find(resolverId);
            if (entry != state->resolvers.end())
                ++entry->postponed;
        }
        return { Solution::Postpone, id };
    }

    void TieBreaker::incomingFinished(quint64 resolutionId, RemoteResult result)
    {
        if (!resolutionId)
            return;
        const auto state      = state_;
        auto       resolution = state->resolutions.find(resolutionId);
        if (resolution == state->resolutions.end() || resolution->remoteResult)
            return;
        resolution->remoteResult = result;
        state->tryRetry(resolutionId);
    }

    void TieBreaker::clear()
    {
        ++state_->epoch;
        state_->currentOutgoing = 0;
        state_->transactions.clear();
        state_->resolutions.clear();
        for (auto it = state_->resolvers.begin(); it != state_->resolvers.end(); ++it)
            it->postponed = 0;
    }

}}
