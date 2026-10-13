# XEP-0284: Shared XML Editing — end-to-end protected payload in messages

Written against XEP-0284 version 0.1.4, XEP-0384 OMEMO 2 and XEP-0420 SCE.

## Problem

SXE payload in `<message/>` stanzas is readable by the servers. A Jingle connection with
verified DTLS is end-to-end protected, but not every client or network has one; the message
transport is the baseline every SXE client has.

## Proposal

- A client advertises `urn:xmpp:sxe:0#omemo` when it can protect SXE payload with OMEMO 2.
- The Jingle SXE transport carries the agreed method; the answer repeats it:

  ```xml
  <transport xmlns='urn:xmpp:jingle:transports:sxe' encryption='urn:xmpp:omemo:2'>
    <host>romeo@montague.lit/orchard</host>
  </transport>
  ```

- Each `<sxe/>` then travels as SCE content of an OMEMO 2 message, next to an empty routing marker
  that names the session:

  ```xml
  <message to='juliet@capulet.lit/balcony' id='…'>
    <sxe xmlns='urn:xmpp:sxe:0' session='851ba2'/>
    <encrypted xmlns='urn:xmpp:omemo:2'>…</encrypted>
    <no-store xmlns='urn:xmpp:hints'/>
    <no-copy xmlns='urn:xmpp:hints'/>
  </message>
  ```

  The envelope's content is the complete `<sxe xmlns='urn:xmpp:sxe:0' session id>` element.
- In a protected session unprotected payload is dropped, and payload whose inner `session` differs
  from the marker is dropped.
- Payload is sent and processed in order even though encryption is asynchronous.
- The transport is protected for the purposes of an end-to-end policy; a session that requires
  protection refuses an SXE transport without `encryption`.

## Open questions

- Multi-user whiteboards (see the whiteboard profile proposals) would need OMEMO for groups or a
  group-level key; out of scope here.
- The marker reveals the session id to the servers; the session id is not secret in Jingle anyway.

## Iris

`SXE::PayloadProtector` (`jingle-sxe.h`) and `SXE::SxeOmemo` (`jingle-sxe-omemo.cpp`), tested in
`tests/jingle/sxeomemo.cpp`.
