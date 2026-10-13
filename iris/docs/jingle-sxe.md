# Shared XML Editing (XEP-0284) over Jingle

This document describes the Iris implementation of
[XEP-0284 Shared XML Editing](https://xmpp.org/extensions/xep-0284.html) (Experimental) and the
SVG whiteboard application type: payload in messages or on Jingle connections, Jingle senders
and session proposals with XEP-0353. The general Jingle object model is described in
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
│                                 ├── Document (records ↔ QDomDocument; reparentable)
│                                 └── payload channel: a Connection, or the SXE transport
├── ICE::Pad ──────────────── ICE::Transport ── SCTP data channel (one <sxe/> per message)
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
- Payload goes over a Jingle connection when both sides support it, see
  [Payload on Jingle connections](#payload-on-jingle-connections); otherwise, and as the
  fallback, in messages.
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
(two clients with looped-back streams: synchronization, conflicts, senders, proposals),
`tests/jingle/sxecontentadd.cpp` (a whiteboard added to a session with a file transfer, and a
refused second whiteboard) and `tests/jingle/sxeconnection.cpp` (the same clients with ICE on
localhost: synchronization and edits on an SCTP data channel, including large character data,
broken channels until the fallback to messages, and a whiteboard added to a running file
transfer over ICE that joins its BUNDLE group). Set `SXE_TEST_DUMP=1` to print the
exchanged stanzas. The connection test is skipped when QCA has no DTLS.

## Payload on Jingle connections

### Why

Payload in `<message/>` stanzas travels client → server → client:

- servers shape client-to-server traffic (ejabberd's default `normal` shaper and Prosody's
  `c2s` limits allow a few kilobytes per second), so large snapshots and inline images are slow;
- servers limit stanza size, which forces small `<sxe/>` elements;
- each stanza passes the server's processing (and possibly archives, despite the hints).

### Delivery

The SXE protocol is unchanged; only the delivery differs. SXE needs every element delivered, in
order, as a unit, so the content uses only connections with
`MessageOriented | Reliable | Ordered | DataOriented` features. One message carries one UTF-8
serialized `<sxe/>` element without the `<message/>` wrapper; there is no framing.

| Order | Transport | Notes |
| --- | --- | --- |
| 1 | ICE (`urn:xmpp:jingle:transports:ice:0` or `ice-udp:1`) with an SCTP data channel | Peer-to-peer, TURN fallback. The channel label is the content name, as for file transfer |
| 2 | SXE (`urn:xmpp:jingle:transports:sxe`) | Messages through the server; always available |

The candidates are what `Jingle::Manager::availableTransports()` returns for those features, so
a future transport with reliable, ordered messages qualifies without SXE changes. Not used:

- S5B in TCP mode is a byte stream and would need framing.
- S5B in UDP mode (XEP-0260) would deliver datagrams, but neither reliably nor in order; a lost
  `<new/>` or `<set/>` breaks the document. Iris does not activate UDP mode yet, so every S5B
  connection is a TCP stream. When UDP mode is added, `Connection::features()` must follow the
  connection's mode (`MessageOriented | Unreliable` for UDP) and `addChannel()` must choose the
  mode from the requested features, so that file transfer keeps getting TCP.
- IBB uses the same server path as messages, with IQ overhead.

The `session` attribute stays the Jingle `sid` and `id` stays unique per sender, so buffering,
duplicate detection and `last-id` handling are unchanged. On a connection up to 65536 characters
of edits go into one `<sxe/>` instead of 16384: SCTP messages are limited to 256 KiB by the
association, which also reassembles fragmented messages. An `<sxe/>` that would still exceed
256 KiB is dropped with a warning; edits stay far below that, since `Document` splits character
data into `MaxChdataPerEdit` chunks.

### Negotiation and fallback

- A client that can make such connections advertises `urn:xmpp:sxe:0#connections`
  (`SXE::NS_CONNECTIONS`, proposed). The content creator offers a connection transport only to a
  peer with that feature; otherwise it offers the SXE transport as before. A responder accepts an
  offered connection transport.
- The transport selector is an `NSTransportsList` over the SXE transport and the connection
  transports; the content waits for its channel with
  `expectSingleConnection(MessageOriented | Reliable | Ordered | DataOriented)`. The content
  becomes active when the channel is ready.
- A transport that fails, a channel that is not created, or a channel that closes moves the
  content to the next transport with `transport-replace`: another connection transport if one
  remains, finally the SXE transport. When both sides notice a broken channel at once, both
  propose a replacement and the Jingle tie-break decides; the proposal that lost is withdrawn, so
  `isTransportReplaceEnabled()` allows replacing an SXE transport until it runs.
- On every new channel the joiner sends `<connect/>` again and imports a fresh state, and the
  host sends no edits until then. Edits of the host lost with the broken channel are part of
  the new state; edits of the joiner lost with it, or made during the switch, are lost. This
  reuses the synchronization instead of adding acknowledgements.
- Multi-user sessions will keep using messages (to a MUC room), since peer-to-peer transports
  are one-to-one.

- A content that may use connections returns `allowsSharedTransport() == true`, like file
  transfer and RTP. A whiteboard added with `content-add` to a session whose contents run on ICE
  (a call) joins their BUNDLE group and opens its data channel on the shared association; the
  session's automatic grouping does the rest. A content limited to the SXE transport (the peer
  lacks `urn:xmpp:sxe:0#connections`) stays out of BUNDLE.

### Not done yet

- `Document::MaxChdataPerEdit` is not raised for connections.
- Replacing the transport of a bundled whiteboard after its data channel breaks is not covered
  by a test.

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

The proposals to the profile and to XEP-0284 are in [xsf-proposals/](xsf-proposals/README.md):
the profile changes in `protoxep-svg-whiteboard.md`, payload on connections in
`xep-0284-sxe-connections.md`, and protected payload in `xep-0284-sxe-encryption.md`.
