# XEP-0396: Jingle Encrypted Transports - OMEMO — an envelope for OMEMO 2

Written against XEP-0396 version 0.2.0 (2018-12-06, Deferred) and XEP-0384 OMEMO 2
(`urn:xmpp:omemo:2`).

## Problem

XEP-0396 uses an OMEMO key transport message as the JET envelope: an `<encrypted/>` element
without payload whose per-device encrypted key is the transport key, with the IV in the header.
That only exists in the legacy profile (`eu.siacs.conversations.axolotl`). OMEMO 2 has no key
transport message: its key material is internal to the double ratchet, and every message
payload is an SCE envelope (XEP-0420). XEP-0396 says the `type` attribute matches the XEP-0384
namespace in use, but defines no envelope for OMEMO 2.

## Proposal

With `type='urn:xmpp:omemo:2'` the envelope is an OMEMO 2 `<encrypted/>` element whose SCE
`<content/>` holds the transport secret:

```xml
<security xmlns='urn:xmpp:jingle:jet:0' name='a-file-offer'
          cipher='urn:xmpp:ciphers:aes-256-gcm-nopadding'
          type='urn:xmpp:omemo:2'>
  <encrypted xmlns='urn:xmpp:omemo:2'>
    <header sid='27183'>
      <keys jid='juliet@capulet.lit'>
        <key rid='31415'>…</key>
      </keys>
    </header>
    <payload>…</payload>   <!-- SCE envelope -->
  </encrypted>
</security>
```

SCE envelope (before encryption):

```xml
<envelope xmlns='urn:xmpp:sce:1'>
  <content>
    <secret xmlns='urn:xmpp:jingle:jet-omemo:0' key='BASE64' iv='BASE64'/>
  </content>
  <rpad>…</rpad>
  <from jid='romeo@montague.lit'/>
  <to jid='juliet@capulet.lit'/>
</envelope>
```

- `key` is the transport key (32 bytes for AES-256), `iv` the 12-byte IV.
- The `from`/`to` affixes bind the secret to both parties.
- The cipher with OMEMO 2 is AES-256-GCM; the legacy profile keeps AES-128-GCM.
- A client chooses the profile like for messages: OMEMO 2 when the peer advertises it, otherwise
  the legacy profile.

## Compatibility

Legacy-only peers keep receiving legacy key transport messages. A peer that supports OMEMO 2 for
messages but not inside JET would fail to decrypt the secret and refuse the content; a separate
feature could signal it, but a client adding OMEMO 2 is expected to handle `<encrypted/>` of that
profile wherever it handles the legacy one.

## Iris

Implemented in `Jingle::JetOmemo` (`jingle-jet-omemo.cpp`) and tested in
`tests/jingle/omemojet.cpp` for both profiles.
