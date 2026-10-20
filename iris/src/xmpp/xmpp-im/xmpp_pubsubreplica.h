/*
 * xmpp_pubsubreplica.h - transport-independent XEP-0060 replica reconciliation
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef XMPP_PUBSUBREPLICA_H
#define XMPP_PUBSUBREPLICA_H

#include <QHash>
#include <QString>
#include <QStringList>
#include <QSet>

#include <optional>
#include <utility>

namespace XMPP {

/**
 * Keeps one (service, node) replica consistent with IQ snapshots, live PEP
 * events and locally requested per-item writes. No XML or payload semantics:
 * T must be an owned value, and the caller translates PubSubItem <-> T.
 *
 * This is NOT a compare-and-swap protocol. The server may accept concurrent
 * overwrites of the same item from different clients (XEP-0060 has no
 * mandatory conditional publish). The coordinator prevents *local* stale
 * callbacks, lost buffered pushes and unintended whole-node replacements.
 *
 * Drive beginLoad()/finishLoad() from PubSubItemsTask, apply notifications
 * with published()/retracted(), and send the edits in beginWrite() via
 * PubSubPublishTask/PubSubRetractTask. Always call finishWrite() once the
 * entire batch has settled; a partial failure requires a new server fetch.
 */
template <typename T> class PubSubReplica {
public:
    using Value   = std::optional<T>; // empty = retract
    using Items   = QHash<QString, T>;
    using Changes = QHash<QString, Value>;
    enum class WriteResult { Success, Retry, Rejected };
    using WriteResults = QHash<QString, WriteResult>;

    enum class State { Idle, Loading, Ready, Writing, ResyncRequired };

    struct WriteBatch {
        quint64 epoch = 0;
        quint64 serial = 0;
        quint64 eventSerial = 0;
        Changes changes;
        bool isValid() const { return serial != 0; }
    };

    State state() const { return state_; }
    bool isReady() const { return state_ == State::Ready || state_ == State::Writing; }
    bool isWriting() const { return state_ == State::Writing; }
    quint64 epoch() const { return epoch_; }
    const Items &items() const { return items_; }
    // The user-visible model includes acknowledged data AND all local intents.
    // A newer queued edit supersedes an older in-flight IQ for the same ID.
    Items optimisticItems() const
    {
        Items snapshot = items_;
        const auto apply = [&snapshot](const Changes &changes) {
            for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
                if (it.value())
                    snapshot.insert(it.key(), *it.value());
                else
                    snapshot.remove(it.key());
            }
        };
        apply(inFlight_);
        apply(pending_);
        return snapshot;
    }
    bool hasPendingChanges() const { return !pending_.isEmpty(); }
    bool hasScheduledIntent(const QString &id) const { return pending_.contains(id) || inFlight_.contains(id); }
    bool hasInFlightIntent(const QString &id) const { return inFlight_.contains(id); }
    bool cancelPending(const QString &id) { return pending_.remove(id) > 0; }
    QStringList scheduledIds() const
    {
        QSet<QString> ids;
        for (auto it = pending_.cbegin(); it != pending_.cend(); ++it)
            ids.insert(it.key());
        for (auto it = inFlight_.cbegin(); it != inFlight_.cend(); ++it)
            ids.insert(it.key());
        return ids.values();
    }

    // On reconnect preserve local user intents (including unconfirmed
    // in-flight writes) for revalidation against a new server snapshot.
    // On account changes discard them to avoid crossing authentication scopes.
    // Both modes invalidate all asynchronous IQ tokens from the old session.
    void reset(bool preservePending = false)
    {
        if (preservePending)
            restoreEdits(inFlight_);
        ++epoch_;
        state_ = State::Idle;
        items_.clear();
        if (!preservePending)
            pending_.clear();
        duringLoad_.clear();
        eventRevisions_.clear();
        eventSerial_ = 0;
        activeBatch_ = 0;
        activeLoad_ = 0;
        inFlight_.clear();
    }

    // Can be called after a failed batch to resynchronize from the server.
    quint64 beginLoad()
    {
        if (state_ == State::Loading || state_ == State::Writing)
            return 0;
        state_ = State::Loading;
        duringLoad_.clear();
        return activeLoad_ = ++nextLoad_;
    }

    // Returns false for an out-of-epoch response or a cancelled fetch.
    bool finishLoad(quint64 loadToken, Items snapshot)
    {
        if (state_ != State::Loading || loadToken != activeLoad_)
            return false;
        activeLoad_ = 0;
        for (auto it = duringLoad_.cbegin(); it != duringLoad_.cend(); ++it) {
            if (it.value())
                snapshot.insert(it.key(), *it.value());
            else
                snapshot.remove(it.key());
        }
        items_ = std::move(snapshot);
        duringLoad_.clear();
        eventRevisions_.clear();
        // Preserve serial monotonicity within the same connection.
        state_ = State::Ready;
        return true;
    }

    bool failLoad(quint64 loadToken)
    {
        if (state_ != State::Loading || loadToken != activeLoad_)
            return false;
        activeLoad_ = 0;
        duringLoad_.clear();
        state_ = State::ResyncRequired;
        return true;
    }

    // A push can arrive before the initial items IQ completes. Its content
    // wins over the older snapshot for the affected id.
    void published(const QString &id, const T &value) { applyEvent(id, Value(value)); }
    void retracted(const QString &id) { applyEvent(id, std::nullopt); }

    // The caller provides semantic intents, not an entire stale node snapshot.
    // Repeated edits to one id are coalesced; the newest local intent wins.
    void put(const QString &id, const T &value) { if (!id.isEmpty()) pending_.insert(id, value); }
    void remove(const QString &id) { if (!id.isEmpty()) pending_.insert(id, std::nullopt); }

    WriteBatch beginWrite()
    {
        if (state_ != State::Ready || pending_.isEmpty())
            return {};
        WriteBatch batch;
        batch.epoch = epoch_;
        batch.serial = ++nextBatch_;
        batch.eventSerial = eventSerial_;
        batch.changes = std::move(pending_);
        pending_.clear();
        inFlight_ = batch.changes;
        eventRevisions_.clear(); // Only revisions newer than this batch matter.
        activeBatch_ = batch.serial;
        state_ = State::Writing;
        return batch;
    }

    // Per-item results isolate permanent failures from successful/temporary
    // operations. An absent outcome is conservative: retry after refresh.
    // Partial success still requires an authoritative refresh before retrying.
    bool finishWrite(const WriteBatch &batch, const WriteResults &results)
    {
        if (state_ != State::Writing || batch.epoch != epoch_ || !batch.isValid()
            || batch.serial != activeBatch_)
            return false;
        activeBatch_ = 0;
        inFlight_.clear();
        bool needsRefresh = false;
        for (auto it = batch.changes.cbegin(); it != batch.changes.cend(); ++it) {
            switch (results.value(it.key(), WriteResult::Retry)) {
            case WriteResult::Success:
                if (eventRevisions_.value(it.key()) > batch.eventSerial)
                    break; // Newer server notification takes precedence.
                if (it.value())
                    items_.insert(it.key(), *it.value());
                else
                    items_.remove(it.key());
                break;
            case WriteResult::Retry:
                if (!pending_.contains(it.key()))
                    pending_.insert(it.key(), it.value());
                needsRefresh = true;
                break;
            case WriteResult::Rejected:
                // Never poison the entire queue with an invalid item.
                break;
            }
        }
        eventRevisions_.clear();
        state_ = needsRefresh ? State::ResyncRequired : State::Ready;
        return true;
    }

    bool finishWrite(const WriteBatch &batch, bool success)
    {
        WriteResults results;
        for (auto it = batch.changes.cbegin(); it != batch.changes.cend(); ++it)
            results.insert(it.key(), success ? WriteResult::Success : WriteResult::Retry);
        return finishWrite(batch, results);
    }

    // Node purge/delete cannot safely be interpreted as successful completion
    // of in-flight writes. Ask the owner to reload from the authoritative node.
    void invalidate(bool clearServerItems = false)
    {
        if (state_ == State::Idle)
            return;
        if (state_ == State::Loading) {
            // Ignore its old snapshot: a new fetch will be necessary.
            duringLoad_.clear();
            activeLoad_ = 0;
        }
        if (state_ == State::Writing) {
            // A purge or node deletion invalidates the result of in-flight
            // writes; preserve local intent for reconciliation after a fetch.
            restoreEdits(inFlight_);
            inFlight_.clear();
            activeBatch_ = 0;
        }
        if (clearServerItems)
            items_.clear();
        eventRevisions_.clear();
        state_ = State::ResyncRequired;
    }

private:
    void restoreEdits(const Changes &changes)
    {
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            if (!pending_.contains(it.key()))
                pending_.insert(it.key(), it.value());
    }

    void applyEvent(const QString &id, Value value)
    {
        if (id.isEmpty() || state_ == State::Idle)
            return;
        if (state_ == State::Loading) {
            duringLoad_.insert(id, std::move(value));
            return;
        }
        // Only write acknowledgements need per-ID event revisions; keeping
        // them between batches would grow memory for every observed contact.
        if (state_ == State::Writing)
            eventRevisions_.insert(id, ++eventSerial_);
        if (value)
            items_.insert(id, *value);
        else
            items_.remove(id);
    }

    State state_ = State::Idle;
    quint64 epoch_ = 1;
    quint64 nextBatch_ = 0;
    quint64 activeBatch_ = 0;
    quint64 activeLoad_ = 0;
    quint64 nextLoad_ = 0;
    quint64 eventSerial_ = 0;
    Items items_;
    Changes pending_;
    Changes inFlight_;
    Changes duringLoad_;
    QHash<QString, quint64> eventRevisions_;
};

} // namespace XMPP

#endif // XMPP_PUBSUBREPLICA_H
