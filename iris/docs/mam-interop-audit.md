# Iris MAM interoperability audit

Status: **in progress** (started 2026-10-17). Scope: Iris only. Psi history UI, persistence, and account synchronization belong to a subsequent task.

## Sources and scope

Primary standard: [XEP-0313 MAM 1.1.3](https://xmpp.org/extensions/xep-0313.html). Compare behavior actually used by at least one of:

- [Dino `xmpp-vala/src/module/xep/0313_message_archive_management.vala`](https://github.com/dino/dino/blob/master/xmpp-vala/src/module/xep/0313_message_archive_management.vala)
- [Dino `xmpp-vala/src/module/xep/0313_2_message_archive_management.vala`](https://github.com/dino/dino/blob/master/xmpp-vala/src/module/xep/0313_2_message_archive_management.vala)
- [Dino `libdino/src/service/history_sync.vala`](https://github.com/dino/dino/blob/master/libdino/src/service/history_sync.vala)
- [Kaidan](https://invent.kde.org/network/kaidan), which uses [QXmppMamManager](https://api.kde.org/qxmppmammanager.html).
- [Conversations](https://codeberg.org/iNPUTmice/Conversations) and [monocles chat](https://codeberg.org/monocles/monocles_chat): source-by-source audit outstanding. Do not infer every implementation detail merely from monocles' Conversations ancestry.

"Related to MAM" means core archive queries, paging, metadata, synchronization prerequisites, and the interpretation of message elements *replayed via MAM*. It does not imply reimplementing every message-related XEP inside an MAM module.

## Existing Iris implementation: do not duplicate

| XEP / facility | Located in Iris | Follow-up |
| --- | --- | --- |
| XEP-0313 MAM | `xmpp_mamtask.{h,cpp}`, `xmpp_mammanager.{h,cpp}` | **Present but incorrect**; repair in place, not a second MAM manager |
| XEP-0059 Result Set Management | `xmpp_subsets.{h,cpp}` / `SubsetsClientManager` | Reuse for page cursors/limits |
| XEP-0359 Unique and Stable Stanza IDs | `Message::stanzaId(by)`, `stanzaIds()`, `setStanzaIds()`, `addStanzaId()`, `originId()` and `setOriginId()` in `xmpp_message.h` / `types.cpp` | A message may carry IDs issued by multiple entities. No ambiguous no-argument `stanzaId()` or single-ID replacement API is retained; disco trust verification remains a caller responsibility. |
| XEP-0297 Stanza Forwarding | `xmpp_forwarding.{h,cpp}` | Reuse for parsing forwarded message and XEP-0203 delay |
| XEP-0203 Delayed Delivery | `Forwarding`, `Message`, presence parsing | The original code truncated UTC offsets/fractions and appended an extra `Z` when serializing. Fixed and tested on `ai/xep0359-multiple-stanza-ids` (Qt5/Qt6). |
| XEP-0280 Message Carbons | `xmpp_carbons.cpp`; `Message::forwarded()` | Keep separate from archive-result envelope handling and deduplication |
| XEP-0334 Message Processing Hints | `Message::processingHints()` | Preserve hints when replaying messages |
| XEP-0308 Last Message Correction | `Message::replaceId()` | Replayed correction is not a second user message |
| XEP-0424 Message Retraction | Handling found in `types.cpp` | Verify semantics against archived originals/tombstones |
| XEP-0444 Message Reactions | Handling found in `types.cpp` | Verify replay of reactions and deduplication |
| XEP-0425 Message Moderation | No dedicated IQ API found | Dino's implementation sends a moderation IQ; this is a distinct MUC operation, not required to parse ordinary MAM result envelopes. XEP-0424 retraction handling already exists. |
| XEP-0421 Occupant IDs | No matching namespace found in first source search | Validate MUC identity and archived sender handling |
| XEP-0461 Message Replies | No matching `urn:xmpp:reply:0` found in first source search | Check any alternate parsing implementation before adding |
| XEP-0490 Message Displayed Synchronization | Generic PubSub manager exists, no typed MDS API | Dino publishes a per-conversation `displayed` PubSub item containing XEP-0359 IDs. This is not a MAM transport extension; do not add it to MAMTask. |
| XEP-0441 MAM Preferences | Not found in first MAM source search | Optional protocol, not required for baseline sync; specification discourages use where it interferes with synchronization |
| XEP-0442 PubSub MAM | Not found in first MAM source search | Include only if verified in scope client; avoid speculative support |

Dino's `xmpp-vala/src/module/xep` contains implementations of XEP-0059, -0203, -0280, -0297, -0313, -0308, -0334, -0359, -0421, -0424, -0425, -0444, -0461 and -0490. The last entries are **candidate protocol dependencies for correct history replay**, not necessarily MAM transport features. Their exact implementation and Iris support need detailed comparison.

## Confirmed defects in existing Iris MAMTask

1. `MAMTask::take` ignores the final IQ `<fin/>` and does not use its `complete` attribute or RSM `<first/>` and `<last/>` cursors. XEP-0313 requires finishing a page on this IQ, not on a guessed number of result stanzas. It can hang on short pages.
2. `getPage` passes `mamMaxMessages` to RSM `setMax` instead of `mamPageSize`; limits are conflated. `messagesFetched >= mamMaxMessages` also fires immediately when `mamMaxMessages == 0`, despite documented unlimited semantics.
3. `getPage` can issue the next page on receiving result number `mamPageSize`, **before** receiving the current query's `<fin/>`. This makes query/result correlation and termination unreliable.
4. `onGo()` unconditionally requests `<metadata/>`. This operation requires `urn:xmpp:mam:2#extended` discovery support; ordinary MAM servers must still work without it.
5. `flip-page`, `after-id`, `before-id`, and `include-groupchat` are sent without checking advertised extended/groupchat-field features. XEP-0313 explicitly gates these.
6. Time values use `QDateTime::toString()` without `Qt::ISODate`; the wire format must be XEP-0082 / UTC ISO-8601. `setTimeSpec(Qt::UTC)` also changes an instant's interpretation rather than converting with `toUTC()`.
7. The input JID is always used as a `with` filter. The IQ itself has no archive `to` address, so querying a MUC room archive is not correctly represented. Empty `with` must be omitted for unfiltered catch-up.
8. `MAMTask(const MAMTask&)` aliases the owned raw `Private*` and both destructors delete it. Since `Task` inherits `QObject`, copying must be disabled.
9. The ID-range `get()` overload fails to initialize `Private::q` (used by `getPage` and `getArchiveMetadata`), and both `get` overloads leave fields from the other mode untouched. Initialization/reset invariants need a single internal path.
10. `archive()` buffers raw `QDomElement` values without explicit result metadata or streaming; evaluate bounds, cancellation, and lifetime before relying on it for unbounded catch-up. No dedicated MAM regression tests were found under the current `tests/` directory.

## Proven baseline from Dino

Dino keeps protocol operations separate from the application's `HistorySync`: it creates an IQ addressed to `mam_server`, correlates message results by `queryid`, reads `<fin/>` with the RSM first/last and completion flag, and checks the claimed archive sender before dispatching the forwarded messages. Dino's `HistorySync` then maintains per-account, per-archive ranges and deduplicates messages by archive ID.

**Iris architecture choice:** Keep query construction, result validation, `<fin/>` pagination and optional streaming callback in the existing `MAMTask` / `MAMManager`. Reuse existing `SubsetsClientManager`, `Forwarding`, `Message`, disco and Task infrastructure. Leave persistent cursors, local DB ranges, storage and UI synchronization to Psi; do not create an Iris-level history database.

## Client-source comparison (2026-10-17)

This comparison distinguishes **protocol transport** from **application synchronization and message replay**.

| Client / inspected source | Confirmed archive behavior | Iris coverage on PR #118 |
| --- | --- | --- |
| [Dino MAM module](https://github.com/dino/dino/blob/master/xmpp-vala/src/module/xep/0313_message_archive_management.vala) + [HistorySync](https://github.com/dino/dino/blob/master/libdino/src/service/history_sync.vala) | MAM v2, own/MUC archive JID, queryid correlation, sender checks, RSM first/last, per-archive range tracking, persisted archive IDs | Wire protocol and streaming results covered; DB ranges remain application responsibility |
| [Kaidan GitHub mirror](https://github.com/KaidanIM/Kaidan/blob/master/src/MessageHandler.cpp) | QXmppMamManager; time-based catch-up, per-contact initial/backlog, reverse RSM, streaming `archivedMessageReceived`, completion callback | Query filters, paging, streaming and fin state covered; storage/UI remain application responsibility |
| [Monocles DinoXIM development fork](https://git.dinox.im/dinoxim/monocles-chat/src/branch/master/src/main/java/eu/siacs/conversations/services/MessageArchiveService.java), modified 2026-10-04 | MAM namespaces 0/1/2, account/MUC discovery, catch-up, reverse paging, per-conversation cursors, sender verification | MAM2 behavior covered; legacy 0/1 namespaces still to assess |
| [Conversations-derived public fork](https://github.com/r4sas/Conversations-I2P/blob/master/src/main/java/eu/siacs/conversations/services/MessageArchiveService.java) | MAM 0/1/2, own/MUC archive routing, timestamp/cursor catch-up and page lifecycle | Modern MAM2 covered; legacy compatibility open |

**Source provenance:** the monocles link is a current, public development fork, **not** the canonical Codeberg monocles repository; the Conversations link is likewise a fork. The canonical Codeberg trees remain to be checked independently. Do not claim an exhaustive upstream audit based on these forks.

## Implementation status in draft PR #118

Completed in Iris's **existing** `MAMTask`, without a second manager:

- Fixed query initialization, disallowed copying the QObject-backed task, reset mutually exclusive filter modes and serialized timestamps as UTC ISO-8601.
- Changed page lifecycle to wait for `<fin/>`; use RSM `first`/`last`, `complete` and cumulative `stable`. Detect repeated cursors and honor page size vs total cap, including unlimited mode.
- Separated the archive JID from the optional `with` filter; own-account and MUC room archives can be queried independently.
- Applied disco feature gates to `flip-page`, `after-id`, `before-id` and `include-groupchat`. For compatible one-sided ID queries, use baseline RSM before/after as allowed by XEP-0313 §9.
- Verified exact archive sender JID and queryid for results and final IQ; ignore unsolicited, forged and post-completion results. Regression tests cover forged own-resource completion IQs.
- Fixed a dispatcher bug: `JT_PushMessage` previously swallowed MAM result envelopes before `MAMTask` could receive them.
- Added `resultReceived(const QDomElement&)` and `setCollectResults(false)` for streaming catch-up without accumulating the entire archive; existing `archive()` remains backward compatible by default.
- Added `tests/mam/mamtask.cpp` with a real Client/Task/recording-stream fixture for own/MUC archives, forged senders and IQ completion, baseline and extended queries, RSM paging, total limits, streaming, `complete`/`stable`, IQ errors and repeated cursors.

**CI:** Dedicated MAM regression jobs on Qt5/QCA2 and Qt6/QCA3 passed on `e19915a` (2026-10-17), including actual CTest execution. The earlier cross-platform `Build and test` matrix passed all seven jobs on `be7b937`. The latest security/edge-case commit `ffe52da` is being retested; keep the PR **Draft** until the updated matrix finishes.

**Outstanding:** canonical Conversations/monocles verification; legacy MAM 0/1 decision; optional MAM metadata and `ids` retrieval only if a scoped client uses them; malformed forwarded-result tests and sanitizer coverage. XEP-0359 multi-ID and XEP-0203 timestamp fixes are isolated in the separate message-layer branch `ai/xep0359-multiple-stanza-ids` (both new tests pass Qt5/Qt6). XEP-0421 occupant IDs and XEP-0461 replies still need implementation decisions, without inflating MAMTask.

## Implementation sequence and acceptance

1. Repair and test existing MAM task lifecycle, copy safety, page size/total limits, correct RSM cursor semantics, timestamp encoding, `<fin/>` completion, IQ error propagation, result filtering and cancellation. Test both forward and reverse paging, partial final pages, empty archive, unlimited mode and unknown query IDs.
2. Support unfiltered own-account catch-up and MUC room archives by separating **archive JID** from optional **with-filter JID**; choose features by disco, not hard-coded assumptions. Regression-test normal-only and extended-capable servers.
3. Add optional extended features only when advertised: archive metadata, ID bounds/single-item retrieval, flipped pages and groupchat field. Keep legacy signatures working where possible and avoid inflating public headers.
4. Audit security and semantic replay: correct origin verification, stable-ID trust and deduplication; use existing forwarded/delay/carbon/message parsers. Add missing, demonstrably used message-XEP parsers outside MAM only when genuinely absent.
5. Cross-check current Conversations, monocles, Dino and Kaidan sources; run Qt5/Qt6, unit and sanitizer CI; document tested interop. No Psi integration until separately requested.

Completion requires passing tests and an audited comparison, **not** merely adding namespace strings or declaring feature support.


## Audit follow-up — 2026-10-17 (work retained in Git)

The earlier implementation-status snapshot above is historical; use the current
PR heads and CI for authoritative status.

- **PR #118** (`ai/mam-interoperability-audit`): XEP-0313 tombstone results
  are accepted without a forwarded `<message/>` or `<delay/>` (both are
  SHOULD, not MUST). Present elements are validated, archive result UID
  remains available, and regression cases cover malformed present elements,
  missing message/delay and empty forwarding. The existing `MAMManager`
  returns a configurable `MAMTask` **before** `go()`; clients set
  `setArchiveFeatures()` for reverse `after-id` queries, which require
  `#extended`. This contract now has a regression test; no new manager API.
- **PR #119** (`ai/xep0359-multiple-stanza-ids`): XEP-0359 multiple
  stanza IDs, XEP-0203 timestamps and XEP-0461 replies; input parsing
  distinguishes an ordinary stanza `id` from explicit XEP-0359
  `<origin-id/>`, preserving outgoing ID generation. Tests cover that
  distinction and modern/legacy delayed presence timestamps.
- **PR #120** (`ai/xep0421-occupant-ids`, stacked on PR #119): XEP-0421
  opaque MUC occupant IDs in existing `Message` and `Status`, including
  MAM-replayed messages and presence. XML parsing enforces at most one direct
  occupant-id and a maximum of 128 characters. The value remains
  **untrusted** until a caller verifies the specific room's
  `urn:xmpp:occupant-id:0` disco#info feature. This is intentionally not
  a second MUC identity layer.

Additional capabilities confirmed in Dino's current XMPP modules:

- **XEP-0421:** `xmpp-vala/src/module/xep/0421_occupant_ids.vala`.
- **XEP-0424 message retraction:** already parsed by Iris's existing
  `Message` (`urn:xmpp:message-retract:1`); do not reimplement.
- **XEP-0425 moderated message retraction:** Dino has
  `0425_message_moderation.vala`. Iris support remains to be assessed
  outside MAMTask, using existing IQ/Task and MUC machinery.
- **XEP-0490 message displayed synchronization:** Dino has
  `0490_message_displayed_synchronization.vala`. This belongs in
  existing PEP/PubSub and stanza-ID layers, not MAMTask. Verify PEP
  publication options/access restrictions and sender attribution before
  marking read positions; never regress cursors.
- Dino's MAM modules use `urn:xmpp:mam:2`; legacy MAM 0/1
  compatibility is not justified solely by Dino. Evaluate only after
  checking a current targeted client and expected server support.

**Source provenance:** `dino/dino` is upstream on GitHub; Kaidan's active
upstream is `invent.kde.org/network/kaidan`, rather than its archived
GitHub mirror. Public Conversations-derived and monocles development forks
do not establish their canonical Codeberg upstream feature set.

All three PRs remain Draft until current Qt5/Qt6 and cross-platform CI,
protocol review, and the remaining interop comparisons are complete.
Psi integration is outside this audit.

## Follow-up implementation status — 2026-10-17 10:30 UTC

This section supersedes earlier *open gap* descriptions; all branches below
are Draft and deliberately not merged. CI status is a snapshot, not a claim
about future commits.

- **PR #118**, `ai/mam-interoperability-audit`: namespace-prefixed MAM
  `result`, `forwarded`, `delay`, `message`, `fin`, RSM `set` and
  `first/last` handling now follows local names plus XML namespaces.
  Regression tests include paginated prefixed RSM; the latest matrix is
  running. Existing MAM paging, tombstones, manager contract and streaming
  functionality are kept in the original classes.
- **PR #119**, `ai/xep0359-multiple-stanza-ids`: multiple stanza IDs,
  origin-id parsing, XEP-0203 timestamps and XEP-0461 reply support;
  previously green on Qt5/Qt6 and full CI.
- **PR #120**, `ai/xep0421-occupant-ids`, based on #119: MUC occupant
  identifier parser/serializer. The 128-character limit now counts Unicode
  code points rather than UTF-16 code units; 128/129 non-BMP character
  regressions cover messages and presence. An occupant ID remains raw until
  the **specific room** advertises `urn:xmpp:occupant-id:0`.
- **PR #121**, `ai/xep0490-displayed-sync`, based on #119: XEP-0490
  `MessageDisplayedSync` XML payload parser/serializer, secure PEP
  publication options, sender and stanza-ID issuer validation. Uses
  existing `PubSubManager::publish/items`; no new PubSub/MAM manager or
  persistence. The application MUST check the account's disco for
  `http://jabber.org/protocol/pubsub#publish-options` before publication,
  accept notifications only from its own bare JID, resolve referenced
  stanza IDs against local history and never move the displayed cursor
  backwards. The PR's Message Qt5/Qt6 and full build matrix passed.
- **PR #122**, `ai/xep0425-moderated-retraction`, based on #120:
  `ModeratedRetractionTask` sends an XEP-0425 IQ to the bare MUC JID
  using the room-issued stanza-ID, sends optional reason and propagates IQ
  result/error. Existing Task/IQ infrastructure is reused; the caller
  checks room disco and moderation eligibility.
- **PR #123**, `ai/xep0425-moderation-metadata`, based on #122:
  `Message::ModeratedRetraction` models moderated MUC action vs archived
  tombstone, moderator JID, occupant ID, reason and timestamp. Tombstones
  never fabricate a live retraction action or a target ID; the MAM result
  UID identifies the archived original. A forged moderated action from a
  MUC occupant cannot be processed via the legacy XEP-0424
  `Message::retraction()` accessor. Room identity, advertised features and
  MAM archive provenance remain the application's trust responsibility.

**Remaining work:** full CI and review for the latest #118/#120/#122/#123
heads; canonical upstream Conversations/monocles/Kaidan verification; any
demonstrably needed remaining MAM compatibility. No claim of end-to-end
application read-state synchronization: UI history lookup, storage and
monotonic cursor changes belong to the consuming application, not Iris.


## MAMTask request/execution lifecycle (review follow-up)

`MAMTask` is a **one-shot** `Task`, not a reusable archive query object. A
completed `Task` cannot be restarted because its base-class completion state
is terminal. The two existing `get()` overloads are retained for API
compatibility, but they now only replace a `Request` value before `go()`:

- `Request` stores the chosen, mutually exclusive time/ID filter and paging
  options. Reconfiguring before `go()` replaces the whole request rather than
  leaving stale fields from the other filter mode.
- `Progress` stores active query/IQ IDs, effective baseline-RSM boundaries,
  cursor history, message count, final flags and buffered results. It is
  initialized by `onGo()`, never by `get()`.
- An ID-range fallback to baseline RSM modifies only the *effective*
  `Progress` cursor, not the originally configured range.
- The archive address, discovered capabilities and collection policy must be
  provided before `go()`. They are frozen once execution starts so a
  mid-query change cannot redirect sender validation.
- `Private` no longer stores `MAMTask *q`; the task itself owns IQ construction,
  sending, and caller-specific methods. PIMPL lifetime uses `std::unique_ptr`.

Regression coverage checks switching ID/time filters prior to `go()`,
no archive reset from a rejected in-flight `get()`, and rejection of a
mid-query change of the archive JID. Do not attempt to reuse a finished
`MAMTask`; request a new task from `MAMManager`.


## Page-task hierarchy refactor (2026-10-17)

The original MAMTask took responsibility for both multi-page orchestration and
parsing IQ responses. That contradicted Iris's hierarchical Task dispatch:
`Task::take()` delegates each stanza to its child tasks, which own correlation.

**Current design in PR #118:**

- `MAMTask : Task` is a **one-shot coordinator**. It freezes the configured
  Request when started, owns aggregate Progress (pagination direction,
  visited cursors, message count, cumulative `complete` / `stable`, result
  buffering), and creates one child for each page. Its `take()` only invokes
  the inherited `Task::take()` for its children.
- `MAMPageTask : Task` is entirely private to `xmpp_mamtask.cpp`. Each
  instance constructs and sends exactly **one MAM IQ**, correlates the page
  query ID with streamed MAM result messages, verifies sender and exact IQ
  response, parses `<fin/>`, and signals `finished()` on completion.
- The coordinator consumes validated page entries via a callback, subject to
  the total result limit, and connects to the child's existing `finished()`
  signal. It starts the next page only after receiving the previous page's
  `<fin/>`, using RSM edge cursors extracted by that child.
- A completed child is deactivated **synchronously**, then scheduled for
  deletion with `safeDelete()`. A delayed result or IQ from an older page
  cannot be consumed by that child while it awaits `deleteLater()`.
- Parent completion (including timeout/disconnect) also deactivates the
  outstanding page, avoiding late callbacks on a completed coordinator.
- Parsing of forwarded messages continues to use Iris's existing message
  and forwarding implementation; no second MAM message parser or public
  page type was introduced.

Tests assert the active MAM child is a Task, each subsequent page gets a
distinct child, retired pages decline stale results / IQs, and MAM never
swallows unrelated IQs from sibling tasks. Tests cover Qt5 and Qt6.

The earlier Request/Progress separation above still applies, but **IQ
generation and IQ validation now belong exclusively to MAMPageTask**, not
to MAMTask. This section supersedes earlier descriptions of a coordinator
sending its own page IQ.

## Audit follow-up — 2026-10-17 12:30 UTC

- The page-task hierarchy originally inherited `Task`'s 120-second timeout
  for **both** its parent coordinator and each child page. This terminated
  otherwise healthy multi-page queries after 120 seconds in total. The
  coordinator now uses `setTimeout(0)` by default while each child retains
  the normal 120-second page deadline. A caller can opt into a separate global
  deadline via the inherited `setTimeout(N)`. Regression tests cover both
  defaults and the explicit override.
- `iqVerify()` intentionally validates stanza type/name, response id and
  generic addressing, **not** whether an IQ is a `result/error` response.
  The private `MAMPageTask` additionally validates exact archive sender and
  `type=result/error`, without reimplementing the generic checks. The
  XEP-0425 moderation IQ handler in sibling Draft PR #122 was separately
  fixed to apply the same response-type rule and stop accepting replies after
  completion; this functionality is not duplicated in MAMTask.
- Current Kaidan primary source is KDE Invent `network/kaidan`. Its
  [official KDE GitHub mirror](https://github.com/KDE/kaidan/blob/master/src/MessageController.cpp)
  confirms own/MUC MAM retrieval, RSM `before` for backlog and `after` for
  catch-up, and selecting XEP-0359 stanza IDs by expected issuer. The older
  `KaidanIM/Kaidan` mirror should not be treated as current upstream.
- Canonical Codeberg Conversations and monocles trees remain inaccessible
  from the available web source, so comparisons based on forks must remain
  explicitly qualified. No claim of canonical source-level completion.

## Subscriber pipeline follow-up (2026-10-17 15:00 UTC)

MAM envelopes now enter the normal JT_PushMessage Stanza/Message parser. A private MAMPageTask subscriber consumes only matching active-page results. Other messages, including unknown query IDs, continue to ordinary subscribers and consumers. SubscriberResult distinguishes NotHandled, HandledContinue, HandledStop and Error while preserving the legacy bool virtual method. Qt5 MAM regression currently fails with a segfault; Qt6 passed. Diagnose before merging.

## Message subscriber integration (2026-10-17)

MAM `<result/>` is carried inside a **regular XMPP message stanza**.
It must not bypass `JT_PushMessage`, stanza construction/validation, XML
subscribers, or `Message::fromStanza()`. The temporary early `return false`
for MAM envelopes in `JT_PushMessage::take()` has been removed.

- `Message::MamResult` retains the archive result ID, query ID, and the
  original result element; `Message::fromStanza()` recognizes the envelope.
  Existing `MAMTask::archive()` and streaming result XML remain available.
- The private, **one-IQ-per-page** `MAMPageTask` also subscribes to the
  existing `JT_PushMessage` message dispatch chain. It checks active page,
  query ID, exact archive sender and forwarding shape, then reports its
  result and returns `HandledStop`. Page IQs/`fin` remain Task-routed.
- An unknown query ID, nonmatching sender, or inactive page returns
  `NotHandled`: the message continues to other subscribers and normal
  application delivery. Iris must not introduce an extra global UI filter;
  the consumer decides whether it is displayable.
- `JT_PushMessage::SubscriberResult` distinguishes `NotHandled`,
  `HandledContinue`, `HandledStop` and `Error`. The legacy virtual
  bool `messageEvent` remains bridged (false = NotHandled, true =
  HandledStop), preserving existing Carbons, Forwarding and PubSub subscribers
  until they explicitly opt into richer results. XML subscriber mutation and
  removal semantics are unchanged.
- Test fixtures must retain their owning `QDomDocument` for the lifetime
  of received `QDomElement` nodes. Returning an element from a destroyed
  local document caused a Qt5 crash in `Stanza::lang()` once MAM messages
  were routed through the standard parser; it was not evidence that the new
  message dispatch path itself needed to be bypassed.
- XEP-0059 RSM request building still reuses Iris's existing
  `SubsetsClientManager` in `xmpp_subsets.{h,cpp}`; no second RSM
  implementation has been introduced.
- Generic XML lookup belongs in `xmpp_xmlcommon.{h,cpp}`, not private
  MAM-specific helpers. `firstChildElementByLocalName()` replaces
  the local `directChildNamed()`: it only examines direct children and
  preserves the original prefix-insensitive matching semantics. The
  preexisting `childElementsByTagNameNS()` remains the correct choice
  when the namespace must participate in element selection. Regression
  tests exercise both helpers.

The previous Qt5 fixture lifetime crash was fixed and Qt5/Qt6 MAM and
cross-platform CI were all green on `df786e27`. The subsequent shared XML
helper refactor still requires its own CI check. This PR remains Draft
pending that validation, nested forwarding and canonical client-source audit.
