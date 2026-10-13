# XEP-0284: Shared XML Editing — payload on Jingle connections

Written against XEP-0284 version 0.1.4 (2020-03-10, Experimental) and XEP-0166.

## Problem

XEP-0284 sends `<sxe/>` payload in `<message/>` stanzas through the servers. Server shapers allow
a few kilobytes per second and limit stanza sizes, so large snapshots (images in a whiteboard)
are slow and must be split into many small stanzas.

## Proposal

- A client that can carry SXE payload on Jingle connections advertises
  `urn:xmpp:sxe:0#connections`.
- The SXE content may then use any Jingle transport that offers reliable, ordered,
  message-oriented connections (today ICE with an SCTP data channel, XEP-0343). Each message is
  one UTF-8 serialized `<sxe/>` element without the `<message/>` wrapper; there is no framing.
  The data channel label is the content name.
- Stream transports (S5B TCP) are not used: they would need framing. Unreliable datagrams (S5B UDP)
  are not used: a lost edit breaks the document.
- The `session` attribute stays the Jingle `sid`, `id` stays unique per sender.
- When the connection breaks, `transport-replace` moves the content to the next transport,
  finally the SXE message transport. After every replacement the joiner sends `<connect/>` again
  and imports a fresh state; the host sends no edits until then. Simultaneous replacements are
  settled by the XEP-0166 tie-break.
- A content that may use connections can share a BUNDLE association (RFC 9143) with a call.

## Iris

Implemented (`jingle-sxe.cpp`), tested in `tests/jingle/sxeconnection.cpp`; see
[../jingle-sxe.md](../jingle-sxe.md).
