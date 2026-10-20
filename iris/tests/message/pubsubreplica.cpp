/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../src/xmpp/xmpp-im/xmpp_pubsubreplica.h"

#include <QCoreApplication>
#include <QString>

using Replica = XMPP::PubSubReplica<QString>;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static void snapshotAndEvents()
{
    Replica replica;
    const auto epoch = replica.beginLoad();
    check(epoch != 0 && replica.state() == Replica::State::Loading, "Load did not start");
    replica.published(QStringLiteral("new"), QStringLiteral("push-new"));
    replica.retracted(QStringLiteral("deleted"));
    replica.published(QStringLiteral("same"), QStringLiteral("newer"));
    check(replica.finishLoad(epoch, {
        { QStringLiteral("deleted"), QStringLiteral("old") },
        { QStringLiteral("same"), QStringLiteral("stale") },
        { QStringLiteral("untouched"), QStringLiteral("keep") }
    }), "Load failed");
    check(replica.items().size() == 3, "Snapshot+push cardinality incorrect");
    check(replica.items().value(QStringLiteral("same")) == QStringLiteral("newer"),
          "Old IQ replaced a newer PEP event");
    check(!replica.items().contains(QStringLiteral("deleted")), "Buffered retraction was lost");
    check(replica.items().value(QStringLiteral("new")) == QStringLiteral("push-new"),
          "Buffered publication was lost");
}

static void queuedEditsAndStaleCallbacks()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(), {
        { QStringLiteral("roomA"), QStringLiteral("old") }
    }), "Initial load failed");
    replica.put(QStringLiteral("roomA"), QStringLiteral("new-local"));
    auto write = replica.beginWrite();
    check(write.isValid() && replica.isWriting() && write.changes.size() == 1,
          "Local write was not scheduled");
    check(!replica.beginWrite().isValid(), "Overlapping batch was allowed");
    replica.put(QStringLiteral("roomC"), QStringLiteral("queued"));
    replica.put(QStringLiteral("roomC"), QStringLiteral("latest"));
    replica.published(QStringLiteral("roomB"), QStringLiteral("other-device"));
    replica.published(QStringLiteral("roomA"), QStringLiteral("newer-push"));
    check(replica.finishWrite(write, true), "Successful batch not confirmed");
    check(replica.items().value(QStringLiteral("roomA")) == QStringLiteral("newer-push"),
          "Late callback overwrote newer remote state");
    check(replica.items().value(QStringLiteral("roomB")) == QStringLiteral("other-device"),
          "Unrelated remote publication was dropped");
    auto second = replica.beginWrite();
    check(second.changes.size() == 1 && second.changes.value(QStringLiteral("roomC"))
              == Replica::Value(QStringLiteral("latest")),
          "Queued changes were not coalesced");
    check(!replica.finishWrite(write, true), "Duplicate callback confirmed a different batch");
    check(replica.finishWrite(second, true), "Queued second batch failed");
    check(replica.items().value(QStringLiteral("roomC")) == QStringLiteral("latest"),
          "Queued edit missing after acknowledgement");
}

static void failedWritesAndReconnect()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(), {}), "Could not load empty node");
    replica.put(QStringLiteral("one"), QStringLiteral("first"));
    replica.remove(QStringLiteral("two"));
    auto batch = replica.beginWrite();
    replica.put(QStringLiteral("one"), QStringLiteral("new-intent"));
    check(replica.finishWrite(batch, false), "Failed batch not consumed");
    check(replica.state() == Replica::State::ResyncRequired, "Failed write did not require reload");
    check(!replica.beginWrite().isValid(), "Write issued before authoritative reload");
    const auto epoch = replica.beginLoad();
    check(replica.finishLoad(epoch, { { QStringLiteral("one"), QStringLiteral("remote") } }),
          "Reload after failure failed");
    auto retry = replica.beginWrite();
    check(retry.changes.size() == 2, "Unconfirmed edits were lost");
    check(retry.changes.value(QStringLiteral("one")) == Replica::Value(QStringLiteral("new-intent")),
          "New user edit did not supersede failed old edit");
    check(!retry.changes.value(QStringLiteral("two")).has_value(), "Deletion intent was lost");
    replica.reset();
    check(!replica.finishWrite(retry, true), "Callback from prior session mutated replica");
    check(!replica.finishLoad(epoch, {}), "Old snapshot applied after reset");
    check(replica.state() == Replica::State::Idle && replica.items().isEmpty(),
          "Account reset leaked old server state");
}

static void undoPendingChanges()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(),
          {{QStringLiteral("existing"), QStringLiteral("old")}}), "Initial fetch failed");

    // A new item that was never sent should disappear from the pending queue
    // rather than generating a useless and possibly rejected retract IQ.
    replica.put(QStringLiteral("new"), QStringLiteral("draft"));
    check(replica.hasScheduledIntent(QStringLiteral("new")), "New item not queued");
    check(replica.cancelPending(QStringLiteral("new")), "Queued insertion not cancellable");
    check(!replica.hasScheduledIntent(QStringLiteral("new")), "Cancelled insert stayed queued");

    // Undo a write already in flight: the earlier IQ may succeed, so an
    // explicit second intent must be sent after its acknowledgement.
    replica.put(QStringLiteral("existing"), QStringLiteral("changed"));
    const auto first = replica.beginWrite();
    check(first.isValid() && replica.hasInFlightIntent(QStringLiteral("existing")),
          "First write did not start");
    replica.put(QStringLiteral("existing"), QStringLiteral("old"));
    check(replica.finishWrite(first, true), "First write completion rejected");
    const auto second = replica.beginWrite();
    check(second.changes.value(QStringLiteral("existing"))
              == Replica::Value(QStringLiteral("old")), "Undo did not override pending update");
    check(replica.finishWrite(second, true), "Undo was not applied");
    check(replica.items().value(QStringLiteral("existing")) == QStringLiteral("old"),
          "Original value not restored");
}

static void optimisticSequentialEditsAndOutcomes()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(), {}), "Initial sync failed");

    replica.put(QStringLiteral("roomA"), QStringLiteral("A"));
    const auto first = replica.beginWrite(); // IQ remains in flight.
    check(replica.optimisticItems().contains(QStringLiteral("roomA")),
          "First unacknowledged room not visible");

    // A second UI edit is built from optimistic state, not the stale server
    // snapshot. Both rooms must survive despite the delayed first IQ.
    auto visible = replica.optimisticItems();
    visible.insert(QStringLiteral("roomB"), QStringLiteral("B"));
    replica.put(QStringLiteral("roomB"), visible.value(QStringLiteral("roomB")));
    check(replica.optimisticItems().size() == 2, "Second edit erased first pending room");

    Replica::WriteResults firstResult;
    firstResult.insert(QStringLiteral("roomA"), Replica::WriteResult::Success);
    check(replica.finishWrite(first, firstResult), "First batch confirmation failed");
    auto second = replica.beginWrite();
    check(second.changes.size() == 1 && second.changes.contains(QStringLiteral("roomB")),
          "Second room was lost while first was in flight");
    check(replica.finishWrite(second, true), "Second batch failed");
    check(replica.items().size() == 2, "Sequential publishes did not converge");

    replica.put(QStringLiteral("bad"), QStringLiteral("invalid"));
    replica.put(QStringLiteral("valid"), QStringLiteral("good"));
    auto mixed = replica.beginWrite();
    Replica::WriteResults results;
    results.insert(QStringLiteral("bad"), Replica::WriteResult::Rejected);
    results.insert(QStringLiteral("valid"), Replica::WriteResult::Success);
    check(replica.finishWrite(mixed, results), "Mixed result not applied");
    check(!replica.optimisticItems().contains(QStringLiteral("bad")),
          "Permanent failure remained in optimistic state");
    check(replica.items().value(QStringLiteral("valid")) == QStringLiteral("good"),
          "One rejected item blocked an unrelated successful item");

    replica.put(QStringLiteral("transient"), QStringLiteral("retry"));
    auto retryBatch = replica.beginWrite();
    Replica::WriteResults retry;
    retry.insert(QStringLiteral("transient"), Replica::WriteResult::Retry);
    check(replica.finishWrite(retryBatch, retry), "Transient failure not consumed");
    check(replica.state() == Replica::State::ResyncRequired,
          "Transient failure didn't force authoritative refresh");
    check(replica.finishLoad(replica.beginLoad(), replica.items()), "Reload before retry failed");
    check(replica.beginWrite().changes.contains(QStringLiteral("transient")),
          "Failed transient item was lost");
}

static void reconnectPreservesUserIntent()
{
    Replica replica;
    check(replica.finishLoad(replica.beginLoad(), {}), "Initial sync failed");
    replica.put(QStringLiteral("inflight"), QStringLiteral("one"));
    auto oldBatch = replica.beginWrite();
    replica.put(QStringLiteral("queued"), QStringLiteral("two"));
    replica.put(QStringLiteral("inflight"), QStringLiteral("newer")); // User changed it again.
    const auto oldEpoch = replica.epoch();
    replica.reset(true);
    check(replica.epoch() != oldEpoch, "Connection epoch was reused");
    check(!replica.finishWrite(oldBatch, true), "Old connection write was acknowledged");
    check(replica.finishLoad(replica.beginLoad(), {}), "Reconnect sync failed");
    auto retry = replica.beginWrite();
    check(retry.changes.size() == 2, "User intents disappeared on reconnect");
    check(retry.changes.value(QStringLiteral("inflight"))
              == Replica::Value(QStringLiteral("newer")), "Stale in-flight write erased newer edit");
    check(retry.changes.value(QStringLiteral("queued"))
              == Replica::Value(QStringLiteral("two")), "Unsent queued edit disappeared");
    replica.reset(); // New account; never leak another account's pending operations.
    check(!replica.hasPendingChanges(), "Account reset preserved old pending writes");
}

static void invalidationAndLoadTokens()
{
    Replica replica;
    const auto firstLoad = replica.beginLoad();
    replica.invalidate();
    const auto secondLoad = replica.beginLoad();
    check(secondLoad != firstLoad, "Separate fetches reused the same token");
    check(!replica.finishLoad(firstLoad, {{QStringLiteral("old"), QStringLiteral("bad")}}),
          "Stale same-session fetch overwrote a newer fetch");
    check(replica.finishLoad(secondLoad, {}), "Fresh fetch result was not accepted");

    replica.put(QStringLiteral("a"), QStringLiteral("intended"));
    const auto firstWrite = replica.beginWrite();
    replica.put(QStringLiteral("b"), QStringLiteral("queued"));
    replica.invalidate(); // e.g. node deletion while a batch is in flight
    check(replica.state() == Replica::State::ResyncRequired, "Invalidation failed");
    check(!replica.finishWrite(firstWrite, true), "Invalidated write was accepted");
    const auto refresh = replica.beginLoad();
    check(replica.finishLoad(refresh, {}), "Load after invalidation failed");
    const auto retry = replica.beginWrite();
    check(retry.changes.size() == 2, "Node invalidation lost in-flight/queued intents");
    check(retry.changes.value(QStringLiteral("a")) == Replica::Value(QStringLiteral("intended")),
          "Original unconfirmed intent was lost");
    check(retry.changes.value(QStringLiteral("b")) == Replica::Value(QStringLiteral("queued")),
          "Newer queued intent was lost");
    check(replica.finishWrite(retry, true), "Retry after invalidation failed");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    snapshotAndEvents();
    queuedEditsAndStaleCallbacks();
    failedWritesAndReconnect();
    invalidationAndLoadTokens();
    reconnectPreservesUserIntent();
    undoPendingChanges();
    optimisticSequentialEditsAndOutcomes();
    return 0;
}
