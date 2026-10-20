# PubSub replica synchronization (design / first consumer)

## Scope

`PubSubManager` remains the XEP-0060 transport. `PubSubReplica<T>` is a
value-typed, transport-independent coordination primitive shared by protocol
clients; it does **not** parse or serialize protocol payloads, dictate storage,
or claim a server-side atomic compare-and-swap operation.

The first intended consumer is XEP-0402 native bookmarks, where
`T = NativeBookmark` and the key is the conference bare JID. OMEMO's aggregate
device list is **not** a drop-in user: its semantic merge of multiple device
IDs inside a single item must remain in OMEMO.

## Rules

1. Bind one instance to one authenticated (account, PubSub service, node).
   Reject events from other senders/nodes before calling its API.
2. Start `beginLoad()` before requesting the server's items IQ. Any live
   +notify events delivered while the IQ is outstanding are buffered by item
   ID; the last event for an ID wins over the snapshot.
3. A local change is a keyed intent `put(id,value)` or `remove(id)`, never a
   replacement for the entire last-read node. Edits while a write is active
   accumulate in `pending_` and coalesce per key.
4. `beginWrite()` starts exactly one concurrent batch. After all item IQs
   settle, call `finishWrite(batch,success)`. On success, a newer PEP event
   for the same key supersedes its older acknowledgement; other keys survive.
5. Even one failing IQ makes a multi-item batch's server state uncertain:
   requeue unsuperseded intents, require a fresh authoritative `beginLoad()`,
   then retry. Never claim the batch was all-or-nothing.
6. On disconnect, `reset(true)` increments the epoch but preserves queued
   and unconfirmed in-flight user intents. Those edits cannot be replayed until
   a new authoritative snapshot is fetched. On account change, `reset()`
   discards them to prevent writes crossing authentication scopes. Both
   invalidate late responses from an earlier epoch.
7. A purge/deletion invalidates the snapshot. Do not infer that earlier
   pending write IQs have failed or succeeded just from the notification.

## Guarantees and limits

`PubSubReplica<T>` provides local callback ordering, stale-IQ protection,
per-item last-event reconciliation, per-item intent coalescing and
failure-triggered reload. It is deliberately in-memory; durable persistence,
encryption and auth policies belong to the consumer.

XEP-0060 usually offers no mandatory conditional publish/CAS. Two independent
clients replacing the *same item* concurrently can still produce lost updates.
No local generation counter can provide a cross-device total order.
Protocol-specific adapters must implement appropriate merge/reload policy
and describe whether convergence is best effort or needs server support.

## Bookmark protocol manager

The first adopter is **`XMPP::BookmarksManager` in Iris**, which contains
the legacy Private XML task, XEP-0402 capability selection and publish/retract
coordination on top of `PubSubReplica<NativeBookmark>`. Its models are
`NativeBookmark` for conferences and `BookmarkUrl` for legacy URL entries.
It has no dependency on Psi, widgets, or per-device application settings.

`PubSubReplica<T>` is intentionally **internal** to Iris until a second real
protocol consumer validates the abstraction; external clients use
`XMPP::BookmarksManager` and its source-aware item events. Applications must
advertise `urn:xmpp:bookmarks:1+notify` in their XMPP caps. The library does
not overwrite the application's full advertised feature set.

The visible conference list is an *optimistic projection*: confirmed server
items, then in-flight IQ intents, then newer queued intents. Consecutive GUI
edits therefore cannot accidentally retract an unacknowledged earlier edit.
`upsertConference()` / `removeConference()` are preferred to replacing the
whole list. Per-item write outcomes distinguish success, transient retry and
permanent rejection; retries use bounded backoff and a single configuration
repair for PubSub `precondition-not-met`.

Psi's former 600-line `BookmarkManager` is now a compatibility adapter which
translates models for existing GUI callers and applies local MUC autojoin
exceptions. `OnlyThisComputer` and `ExceptThisComputer` remain client-local
policies and are deliberately not exported as new protocol values.

A URL bookmark does not have an XEP-0402 representation. To avoid
destructive `storage:bookmarks` replacement on a #compat server, URL entries
are **read-only when native bookmarks are active or were activated earlier in
the session**. Editing remains supported only by the legacy backend.
This limitation must be presented to end users rather than silently ignored.
Full server interoperability tests are still required before merge.

## Migration plan

- Iris: ship the coordinator plus BookmarksManager, leaving OMEMO/JinglePub
  unchanged.
- Psi: keep only the presentation / local MUC preference adapter and UI;
  no application-level Private XML task, PEP subscription, or reconciliation.
- Later, after interoperable bookmark testing, evaluate extracting the
  analogous pieces of OMEMO/JinglePub. Never turn their protocol-specific
  merge semantics into a universal XML merger.

## Required regression matrix

- A push is received while a fetch is outstanding, then a stale IQ completes.
- Item retraction happens while fetching, including an item absent in snapshot.
- Writes happen while another write is in flight, with multiple edits to one ID.
- A server echo for a write arrives before its IQ result.
- A remote change to an unrelated ID arrives during a batch.
- A partial batch fails, is re-fetched, then its intents retry.
- Account reconnect happens before a fetch or write callback returns.
- A node purge during synchronization forces a reload.
- Absent XEP-0402 #compat prevents automatic migration/re-publication.
