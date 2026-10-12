# Shared XML Editing (XEP-0284) over Jingle

This document describes the Iris implementation of
[XEP-0284 Shared XML Editing](https://xmpp.org/extensions/xep-0284.html) (Experimental) and the
SVG whiteboard application type, and records the planned extensions: payload over other Jingle
transports and session proposals with XEP-0353. The general Jingle object model is described in
[jingle.md](jingle.md).

## Specifications

| Specification | Status | Used for |
| --- | --- | --- |
| XEP-0284 Shared XML Editing | Experimental | Record model, `<sxe/>` payload, `urn:xmpp:jingle:transports:sxe` |
| SVG whiteboarding over SXE (minimal profile) | Draft, not submitted to the XSF | `urn:xmpp:jingle:apps:svg-whiteboard:0`, SVG document rules |
| XEP-0166 Jingle | Draft | Session management |
| XEP-0353 Jingle Message Initiation | Experimental | Planned: ringing all resources before the session |
| XEP-0334 Message Processing Hints | Draft | `<no-store/>`/`<no-copy/>` on SXE payload |

The whiteboard profile lives in <https://github.com/jabberworld/whiteboarding>
(`docs/xep-svg-whiteboarding-minimal.xml`, `docs/WHITEBOARD_PROFILE.md`). It was derived from
Psi's historical whiteboard, which used a pre-XEP form of SXE
(`http://jabber.org/protocol/sxe` with a custom `<negotiation/>`). That protocol is no longer
supported. Proposed changes to the profile are listed at the end of this document.

## Current implementation

```text
Session
├── SXE::Pad ──────────────── SXE::Application (one <content/>, description = document type)
│                                 └── Document (records ↔ QDomDocument; reparentable)
└── SXE::TransportPad ─────── SXE::Transport (<host/>; <sxe/> in <message/>)
                                  ▲
                 IncomingSxeTask ─┘ routes <message><sxe session=sid/></message> by (peer, sid)
```

- `SXE::Manager(descriptionNs)` is a generic application manager. `Client` registers it for
  `NS_SVG_WHITEBOARD`; another XML document type only needs another manager instance.
- `SXE::Transport` signals one or more `<host/>` elements and has no `Connection`. Payload goes
  to the Jingle peer as `<message><sxe xmlns='urn:xmpp:sxe:0' session='SID' id='N'/></message>`
  with XEP-0334 `<no-store/>` and `<no-copy/>`. The manager returns `false` from
  `canMakeConnection()` so no other application selects it.
- Only one-to-one sessions are implemented. The creator of the content hosts the document.
- After Jingle acceptance the joiner sends `<connect/>`, accepts the `<state-offer/>` that repeats
  the session description, and imports `<state/>` between `<document-begin/>` and
  `<document-end/>`. Edits received meanwhile are buffered and applied after the snapshot,
  skipping those up to `last-sender`/`last-id`. The host sends no edits to a joiner before its
  snapshot, so its own `last-id` is empty.
- `Document` keeps one record per DOM node and applies the XEP rules: primary/secondary weight
  ordering, removal of orphans and of duplicate roots or attributes with lower secondary weight,
  `target` in `<set/>` and `<remove/>`, no reuse of removed rids, and versioned `<set/>` where a
  conflicting edit reverts the record to version `v - 1`.
- Character data longer than `Document::MaxChdataPerEdit` is split into `<set replacefrom
  replacen='0'/>` edits; outgoing `<sxe/>` elements carry at most about 16 KiB of attribute data.
  In a snapshot the chunks carry the final record version, so the joiner keeps the host's version
  numbers.
- The prolog is a `data:` URI. It is kept as untrusted text and never parsed.
- `Application::document()` is a child of the application. A UI can reparent it to keep the
  content after the session ends; the application then refers to it through `QPointer`.
- A session carries at most one SXE content, since `<sxe/>` payload is addressed by the Jingle
  `sid`. A second one offered by the peer is answered with `content-reject`
  (`incompatible-parameters`). Other applications, such as RTP or file transfer, can share the
  session, and a whiteboard can be added to a running session with `content-add`.
- Jingle `senders` of an SXE content are the participants allowed to edit (see below).
- `SXE::Manager` parses and serializes XEP-0353 proposal descriptions (`SXE::Proposal`), so a
  whiteboard can be proposed through `Jingle::Manager::sendMessageInitiation()`.

Regression tests: `tests/jingle/sxedocument.cpp` (record model), `tests/jingle/sxesession.cpp`
(two clients with looped-back streams: synchronization, conflicts, senders, proposals) and
`tests/jingle/sxecontentadd.cpp` (a whiteboard added to a session with a file transfer, and a
refused second whiteboard). Set `SXE_TEST_DUMP=1` to print the exchanged stanzas.

## Planned: payload over other Jingle transports

### Problem

All SXE payload currently travels client → server → client in `<message/>` stanzas:

- servers shape client-to-server traffic (ejabberd's default `normal` shaper and Prosody's
  `c2s` limits allow a few kilobytes per second), so large snapshots and inline images are slow;
- servers limit stanza size, which forces small `<sxe/>` elements;
- each stanza passes the server's processing (and possibly archives, despite the hints).

### Approach

Keep the SXE protocol and change only the delivery. The whiteboard content may offer any
reliable, ordered Jingle transport; the message-based SXE transport remains the baseline and
fallback.

| Order | Transport | Notes |
| --- | --- | --- |
| 1 | ICE (`urn:xmpp:jingle:transports:ice:0`) with an SCTP data channel | Peer-to-peer, TURN fallback; can share a BUNDLE association with a running call |
| 2 | S5B (`urn:xmpp:jingle:transports:s5b:1`) | Direct or proxied stream |
| 3 | SXE (`urn:xmpp:jingle:transports:sxe`) | Messages through the server; always available |

IBB is excluded: it uses the same server path as messages, with IQ overhead.

Framing on a `Connection`:

- message-oriented channels (SCTP data channel): one UTF-8 serialized `<sxe/>` element per
  message, without the `<message/>` wrapper; channel label `sxe`, protocol `urn:xmpp:sxe:0`;
- stream transports (S5B): each `<sxe/>` element prefixed by its length as a 32-bit big-endian
  integer. A receiver rejects frames larger than a limit (for example 8 MiB) and ends the
  content with a failed-application reason.

The `session` attribute stays the Jingle `sid` and `id` stays unique per sender, so buffering,
duplicate detection and `last-id` handling are unchanged. Frames are not size-limited by the
server, so `Document::MaxChdataPerEdit` and the per-`<sxe/>` budget can be raised for
connection-based delivery.

### Negotiation and fallback

- A peer advertises support for SXE payload on stream transports with a new disco feature
  (proposed: `urn:xmpp:sxe:0#streams`). Without it, only the SXE transport is offered.
- The application selects transports like file transfer: an `NSTransportsList` over
  `{SXE, S5B, ICE}` (least to most preferred), filtered by peer capabilities, and
  `expectSingleConnection(Reliable | Ordered | DataOriented)` for connection-based transports.
- If a connection-based transport fails, Jingle `transport-replace` moves the content to the next
  transport, ending with the SXE transport. In-flight edits may be lost with the failed
  connection, so after any replacement the joiner sends `<connect/>` again and re-imports the
  state. This reuses the existing synchronization instead of adding acknowledgements.
- With an ICE transport the application can return `allowsSharedTransport() == true`; a
  whiteboard added to a call then joins the call's BUNDLE association (requires
  `IRIS_ENABLE_JINGLE_SCTP`).
- Multi-user sessions keep using messages (to a MUC room), since peer-to-peer transports are
  one-to-one.

### Iris changes

- An internal payload channel abstraction in `SXE::Application` with two implementations:
  the existing message channel (`SXE::Transport::sendSxe()`/`sxeReceived()`) and a connection
  channel (framing, `readyRead()`, write queue).
- Transport selection through the selector above; `isTransportReplaceEnabled()` allows
  replacement, and a successful replacement restarts state synchronization.
- `SXE::TransportManager` stays excluded from generic connection selection.
- Tests: the existing session test over S5B and ICE fixtures, a forced transport failure that
  falls back to the SXE transport, and frame-size limits.

## Senders of an SXE content

XEP-0284 does not define what Jingle `senders` means for a shared document. Psi needs it to
switch a whiteboard off without removing it (see Psi's `doc/whiteboarding.md`), so Iris
defines it as the set of participants that may send edits:

- A participant outside `senders` sends no `<new/>`, `<set/>` or `<remove/>`; received edits
  from such a participant are ignored. `<connect/>`, state offers and the state transfer are not
  edits and stay allowed, so a joiner can synchronize a frozen document.
- `senders='none'` freezes the document; the UI hides it. Changing `senders` back continues with
  the same records and versions, without a new state transfer.
- `SXE::Application` returns `supportsContentModify() == true`; changes go through the usual
  `requestSenders()`/`content-modify` machinery and are reported by `sendersChanged`.
  `Application::mayEdit(role)` tells whether a participant may edit, and the document is
  read-only (`Document::isReadOnly()`) while the local participant is not a sender: local
  editing calls are refused and nothing is sent.

## Session proposals with XEP-0353

Calls already use [XEP-0353](https://xmpp.org/extensions/xep-0353.html) through the generic
`Jingle::Manager::sendMessageInitiation()`/`incomingMessageInitiation()` layer. A whiteboard
proposal reuses it:

```xml
<message to='laertes@shakespeare.lit' type='chat' id='p1'>
  <propose xmlns='urn:xmpp:jingle-message:0' id='851ba2'>
    <description xmlns='urn:xmpp:jingle:apps:svg-whiteboard:0'/>
  </propose>
  <store xmlns='urn:xmpp:hints'/>
</message>
```

- `SXE::Manager` implements `parseProposal()`/`serializeProposal()` for its description
  namespace (the minimal profile's description has no children); the payload type is
  `SXE::Proposal`. Psi proposes whiteboards this way (`WbManager`); a proposal can also list RTP
  and SXE descriptions together, which Psi does not use yet.
- All online resources of the peer receive the proposal and answer `<ringing/>`. The resource
  that sends `<proceed/>` becomes the Jingle peer; the initiator then sends `session-initiate`
  with the proposal id as `sid`. `<reject/>`, `<retract/>` and `<finish/>` follow the same rules
  as for calls, including carbons of answers from sibling resources.
- Without XEP-0353 support on the peer (or for a known full JID), the client sends
  `session-initiate` directly, as now.

## Proposed changes to the whiteboard profile

To be discussed with the profile author:

1. Multi-user whiteboards: Jingle with a MUC room as the peer does not work, since IQs to the
   room's bare JID go to the MUC service itself. A MUJI-like model (XEP-0272) fits better:
   advertise the session in room presence (XEP-0284 already defines `<sxe session name/>` in
   presence), send payload as `groupchat` messages to the room, and let the initiator or any
   synchronized participant answer `<connect/>` with a state offer.
2. `<host/>` in one-to-one examples should be the initiator's full JID.
3. Allow history `<set/>` chunks with the same version (for character data split across edits),
   so a joiner keeps the host's record versions.
4. Recommend XEP-0334 `<no-store/>` for SXE messages.
5. Define SXE payload over stream transports (framing, data channel label, disco feature) as
   described above.
6. Recommend XEP-0353 for proposing a whiteboard session to all resources.
7. Define Jingle `senders` for SXE contents as the participants allowed to edit, as described
   above, so a board can be frozen and resumed with `content-modify`.
