# protoXEP: OMEMO-verified DTLS fingerprints in Jingle

Records and extends the protocol that Conversations and Dino implement under the draft namespace
`http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification` (sic). Written against their sources
(Conversations `bf3269cd`, 2026-09-28; Dino `8d49d83b`, 2026-08-21), XEP-0320 and XEP-0353.

## Problem

DTLS protects Jingle media and data channels, but its fingerprint (XEP-0320) travels in Jingle
signaling through the servers. A server can replace it and sit in the middle; DTLS alone is not
end-to-end. Two clients that share an OMEMO session can authenticate the fingerprint with it.

## Current protocol

- The responder of a XEP-0353 proposal names its OMEMO device in `<proceed/>`:

  ```xml
  <proceed xmlns='urn:xmpp:jingle-message:0' id='…'>
    <device xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification' id='31415'/>
  </proceed>
  ```

- In every ICE-UDP transport the fingerprint is replaced by an OMEMO message whose payload is the
  fingerprint text, for that one device:

  ```xml
  <fingerprint xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification'
               hash='sha-256' setup='actpass'>
    <encrypted xmlns='eu.siacs.conversations.axolotl'>
      <header sid='27183'><key rid='31415'>…</key><iv>…</iv></header>
      <payload>…</payload>
    </encrypted>
  </fingerprint>
  ```

- The answer is encrypted for the device in the received header. `content-add` and
  `content-accept` carry encrypted fingerprints too. Dino puts the `<device/>` in MUJI presence.

## Proposal

1. Register a proper namespace (for example `urn:xmpp:jingle:dtls-omemo:0`) and keep accepting
   the draft one.
2. Apply it to every transport with a DTLS fingerprint, including `urn:xmpp:jingle:transports:ice:0`
   and SCTP data channels (XEP-0343), and to `transport-replace`/`transport-accept`.
3. All fingerprints of a session come from one device; a different device is an error.
4. OMEMO 2: with `urn:xmpp:omemo:2`, the SCE `<content/>` holds the whole
   `<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash setup>` element, so `hash` and `setup`
   are authenticated too. The profile follows the peer's capabilities.
5. Policy: a client that requires verification refuses plain fingerprints with `not-acceptable`
   (IQ) and `security-error` (session); without a requirement it falls back to plain fingerprints,
   as deployed. A session sent directly to a full JID without a proposal has no device and cannot
   be verified unless the device is learned otherwise (for example from a previous session).

## Iris

`Jingle::OmemoDtlsVerification` implements the current protocol plus items 2–5, tested in
`tests/jingle/omemodtls.cpp`. See [../jingle-e2e.md](../jingle-e2e.md).
