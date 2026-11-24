# End-to-end encryption for Jingle sessions

Design notes for an "end-to-end required" policy on Jingle sessions: what the deployed clients
do, what Iris has, and how Iris and Psi implement it. Implemented so far: the OMEMO primitives,
the session policy, OMEMO-verified DTLS fingerprints and JET-OMEMO for files in Iris (steps 1–4
of the plan).

## Goal

The application supplies a local protection policy, based on its operation
(e.g. a published file's download policy), and may explicitly select a method
or provide a non-binding method preference. Iris does not own publication access rules
or infer policy from disco alone. The peer's actual offer/answer is distinct from
both local policy and advertised capabilities. Plain DTLS is not authenticated E2EE.


- A session can be marked as requiring end-to-end encryption (for example a call or a whiteboard
  started from an OMEMO chat, or a file sent from it). This is a local policy, not a wire
  attribute: each side decides what it accepts.
- If no end-to-end protected channel can be established, the session tells the user and sends no
  data: no fallback to an unprotected transport. The peer gets `security-error` (XEP-0166).
- No double encryption: a transport that is end-to-end protected by itself (ICE with DTLS whose
  fingerprint is authenticated) is used as is; a transport without its own protection (S5B, IBB,
  SXE payload in messages) gets a protection layer, or is not used.

DTLS alone is not end-to-end: its fingerprint travels in Jingle signaling through the servers, so
a server can replace it and sit in the middle. It becomes end-to-end once the fingerprint is
authenticated with the peer's OMEMO identity.


The Jingle manager observes the client's encryption registry. Registering
the built-in OMEMO method automatically installs its JET and
DTLS-verification adapters; unregistering or destroying the method removes
them. The manager also destroys them before its transport registries.
OMEMO itself does not depend on Jingle, and applications do not need to
enable or manage these adapters explicitly.

An application that explicitly pins a method passes its ID to
`Jingle::Session::setEncryptionMethod(methodId)` **before** starting the
negotiation, along with its desired preferred/required E2EE policy. Iris
creates and holds the associated `EncryptedSession` via the registry,
and releases it when Jingle terminates. A method that is unregistered
while the session is active cannot be silently replaced by OMEMO.

For file publications the application instead leaves the method unbound and
may set `setPreferredEncryptionMethod(methodId)` as a non-binding hint.
JET selects a compatible registered provider at negotiation time, preferring
the hint if compatible, and binds the method before starting the key exchange.
An incoming JET envelope can bind an unspecified method, but cannot replace
an already selected or authenticated method. Failure after provider selection
is terminal. Native DTLS remains preferred where supported; its current
verification adapter is OMEMO.

JET-OMEMO and DTLS verification operate only for an OMEMO-selected (or
legacy unselected) Jingle session. The SXE message transport uses the
selected Jingle `EncryptedSession` to encrypt/decrypt a synthetic message.
Its stateless OMEMO 2 adapter preserves the negotiated wire profile and
extracts the envelope or inner SXE element. No separate registry of
encryption methods or per-payload encryption sessions is needed. A
protected offer may bind a previously unspecified method but cannot
replace a selected one. Unregistration stops protected SXE transports
and suppresses outstanding payloads. When E2EE is required, an unavailable
or mismatched method fails negotiation without sending plaintext.

The existing OMEMO element wrapping for JET keys and DTLS fingerprints
remains method-specific. Binding those element operations directly to
the long-lived Jingle encryption session is the next step; this stage
does not change the JET-OMEMO or Conversations-compatible wire format.

## Specifications

| Specification | Status | Relevance |
| --- | --- | --- |
| XEP-0420 Stanza Content Encryption | Experimental | Can encrypt `<iq/>` as well as `<message/>`; no client encrypts Jingle IQs, so it is not used here |
| XEP-0391 Jingle Encrypted Transports (JET) | Deferred (0.1.2, 2018) | `<security/>` in a Jingle content with an encrypted transport key; examples only for file transfer |
| XEP-0396 JET-OMEMO | Deferred (0.2.0, 2018) | The JET envelope is an OMEMO key transport message; examples use the legacy OMEMO namespace |
| XEP-0320 DTLS-SRTP in Jingle | Stable (1.0.0) | `<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0'/>` in the transport |
| OMEMO DTLS verification | Not a XEP | Conversations' namespace, implemented by Conversations and Dino (below) |

Encrypting whole Jingle IQs with SCE would hide signaling from servers, but no client supports it
and it adds no protection for the media: only the DTLS fingerprint needs authentication.

## What Conversations and Dino do

Read from the sources of Conversations (`bf3269cd`, 2026-09-28) and Dino (`8d49d83b`, 2026-08-21).
Both use the legacy OMEMO profile (`eu.siacs.conversations.axolotl`); neither implements OMEMO 2
(`urn:xmpp:omemo:2`) at these revisions.

### OMEMO-verified DTLS fingerprints (calls)

Namespace: `http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification` (sic, "dlts"). Classes:
Conversations `AxolotlService.encrypt(IceUdpTransportInfo, …)`/`decrypt(…)`,
`OmemoVerifiedIceUdpTransportInfo`, `JingleRtpConnection`; Dino
`plugins/omemo/src/dtls_srtp_verification_draft.vala`.

- In every `<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1'>` the plain
  `<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='…' setup='…'>HEX</fingerprint>` is
  replaced by

  ```xml
  <fingerprint xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification'
               hash='sha-256' setup='actpass'>
    <encrypted xmlns='eu.siacs.conversations.axolotl'>
      <header sid='OWN-DEVICE-ID'>
        <key rid='PEER-DEVICE-ID'>…</key>
        <iv>…</iv>
      </header>
      <payload>…</payload>   <!-- the fingerprint text -->
    </encrypted>
  </fingerprint>
  ```

  `hash` and `setup` stay in clear; the fingerprint value is the OMEMO payload, encrypted for one
  device of the peer.
- The device is agreed through XEP-0353: the responder adds
  `<device xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification' id='OWN-DEVICE-ID'/>`
  to `<proceed/>`, and the initiator encrypts for that device. The responder answers for the
  device in the received OMEMO header (`sid`). Dino also puts the `<device/>` in MUJI presence.
  Without a proposal (direct `session-initiate`) there is no device id and no verification.
- `content-add` and `content-accept` carry encrypted fingerprints too; Dino expects them.
- Only `ice-udp:1` transports are handled by both clients.
- Conversations falls back to a plain fingerprint when encryption fails, unless
  `Config.REQUIRE_RTP_VERIFICATION` is set; then the call fails. Dino marks the content as
  OMEMO-verified when decryption succeeds.

### JET-OMEMO (file transfer)

Disco features `urn:xmpp:jingle:jet:0` and `urn:xmpp:jingle:jet-omemo:0`. Classes:
Conversations `Content.setSecurity()/getSecurity()`, `JingleFileTransferConnection`; Dino
`plugins/omemo/src/jingle/jet_omemo.vala`, `xmpp-vala/src/module/xep/0391_jingle_encrypted_transports.vala`.

```xml
<content creator='initiator' name='…'>
  <description xmlns='urn:xmpp:jingle:apps:file-transfer:5'>…</description>
  <transport xmlns='urn:xmpp:jingle:transports:s5b:1' …/>
  <security xmlns='urn:xmpp:jingle:jet:0' name='CONTENT-NAME'
            cipher='urn:xmpp:ciphers:aes-128-gcm-nopadding'
            type='eu.siacs.conversations.axolotl'>
    <encrypted xmlns='eu.siacs.conversations.axolotl'>
      <header sid='…'><key rid='…'>…</key>…<iv>…</iv></header>   <!-- no payload -->
    </encrypted>
  </security>
</content>
```

- The envelope is an OMEMO key transport message: the encrypted key is the 16-byte AES key, the
  header `<iv/>` the 12-byte IV, for all devices of the recipient.
- The whole file is one AES-128-GCM stream with the 16-byte tag appended at the end
  (Conversations: BouncyCastle `GCMBlockCipher` streams; Dino: `SymmetricCipher("AES-GCM")`
  converter streams). The cipher attribute is written without the `:0` suffix.
- The plaintext is authenticated only at the end, so a receiver must keep the data as unverified
  until the tag matches. Ranged transfers and resumption are not covered.

## Iris today

- `OmemoEncryption` (`xmpp_omemo.h`) implements OMEMO 2 and the legacy profile, but as an
  `EncryptionMethod` it only offers `XmppStanza`. Internally the legacy path already supports a
  message without payload (key transport) and per-device key encryption.
- `OmemoEncryption::encryptElement()`/`decryptElement()` encrypt one element for a peer outside
  of a message and decrypt it, in either profile:
  - only devices of the peer are targets (all, or the given ones), never the account's own;
  - legacy: the element's text is the payload (the fingerprint of Conversations' DTLS
    verification); without an element the result is a key transport message whose key and IV
    are returned as `transportKey`/`transportIv` (the JET-OMEMO envelope);
  - OMEMO 2: the element travels in an SCE envelope bound to both bare JIDs;
  - decryption returns the content (legacy: `<payload/>` with the text, or nothing for a key
    transport message), the sender device, its identity key and its trust level.

  They reuse the message path (device lists, sessions, trust) on a message that is never sent.
  `tests/jingle/omemoelement.cpp` runs them between two clients over an in-memory PEP service:
  one-device encryption and the answer, a forged sender, legacy key transport, and OMEMO 2
  refusing an empty element.
- `Jingle::SignalingFilter` (registered with `Manager::addSignalingFilter()`) rewrites incoming and
  outgoing `<jingle/>` elements asynchronously and adds extensions to outgoing XEP-0353
  messages. While a filter works on an IQ of a session, later IQs of that session in the same
  direction wait; without filters nothing changes. The manager does not
  inspect content XML for particular encryption mechanisms: applications
  and transports enforce the selected content policy before payload I/O. A filter error answers an incoming IQ, or fails
  an outgoing one as if the peer had returned it; a refused `session-initiate` or `session-accept`
  ends the session. `MessageInitiation` keeps foreign extensions (`extensions()`, `addExtension()`).
- `Session::setEncryptionPolicy()` sets the local policy (Disabled, Preferred, Required); `Session::peerAuthentication()` (method,
  device, identity key, trust level) records who authenticated the peer's DTLS fingerprints.
- `Jingle::OmemoDtlsVerification` is registered with `Jingle::Manager` as an optional
  DTLS fingerprint service. The ICE transport parses `<transport/>` and recognizes
  either a plain fingerprint or its protected OMEMO variant. Only the individual
  fingerprint is passed to the service; it does not scan or rewrite complete Jingle
  IQs. The wire format remains compatible with Conversations and Dino:
  - outgoing `<proceed/>` gets our `<device/>`; the device in a received `<proceed/>` is the peer
    device of that session;
  - outgoing DTLS fingerprints of ICE transports (`ice:0` and `ice-udp:1`) are encrypted for that
    device, in OMEMO 2 when the peer advertises it, otherwise in the legacy profile; without a
    device they stay plain, unless the session requires end-to-end encryption;
  - incoming protected fingerprints remain in the parsed ICE update until an asynchronous
    verification completes; only then is the decrypted fingerprint applied to the
    ICE/DTLS association. Signaling IQ acknowledgements do not wait for OMEMO;
    ICE preparation and startup do.
  - verified fingerprints in a session must come from one authenticated device;
    the responder encrypts its own fingerprint to that device;
  - a session requiring end-to-end encryption refuses plaintext fingerprints,
    missing DTLS, untrusted peers and encryption failures with `security-error`.
    A responder can decide its local policy after parsing the incoming
    `session-initiate`, without needing a pre-session cryptographic cache.
- Without a compatible DTLS verification service, required end-to-end encryption
  fails closed unless an application protects its payload separately (e.g. JET
  file transfer). Unprotected RTP and other content is still refused.
  `tests/jingle/omemodtls.cpp` runs a whiteboard on an ICE data channel between two clients in both
  profiles: verified fingerprints only on the wire and a recorded authentication on both sides; a
  session requiring end-to-end encryption without a peer device sends nothing and ends; without
  the requirement fingerprints stay plain; a responder requiring it does not answer a plain
  fingerprint; a tampered fingerprint cannot start DTLS; without a verifier a
  session requiring it sends nothing.
- Capabilities describe supported mechanisms, not the peer's policy. A protected offer
  commits the content to that protection. Disabled does not initiate E2EE but accepts
  compatible protected offers; Preferred permits plaintext only if a compatible
  mechanism is unavailable before negotiation; Required forbids plaintext.
  Signaling filters can add disco features (`SignalingFilter::discoFeatures()`).
- Transport failure disposition is independent of the XEP-0166 reason:
  `TryNextTransport` permits another compatible candidate; `AbortContent`
  terminates the content (authentication failures, identity mismatch, or a lost
  negotiated security mechanism). An authentication failure never triggers plaintext fallback.
  Failed JET key exchange and failed fingerprint encryption after provider selection are
  terminal even under Preferred. Ordinary network failure remains retryable, without
  weakening any previously negotiated protection.
- `Jingle::JET::Manager` is an always-present Jingle service that registers
  XEP-0391 key-delivery providers and advertises their disco features. It does
  not own Jingle sessions or create OMEMO providers. `JET::Pad` belongs to a
  Jingle session and selects a provider using its negotiated encryption method.
  `JET::Protection` belongs to one content, retains its own envelope and keys,
  and prepares them asynchronously before the transport is offered or accepted.
  No Jingle-IQ pre-parse JET filter or (peer, sid) pending-key cache exists.
- `JET::Connection` wraps a `Jingle::Connection::Ptr`: FileTransfer receives
  plaintext through the same interface irrespective of ICE, S5B or IBB.
  The wrapper handles AES-GCM, the final 16-byte tag, data framing and errors.
  One content key/IV may protect only one stream: transport replacement cannot
  silently reuse that pair. This implementation requires the exact plaintext
  length. Known-length ranges (including offset-to-end when the size is known)
  use a fresh content key and IV. Indefinite-length streams are not supported.
- FileTransfer prefers authenticated native transport encryption when the
  transport can negotiate it; JET remains the fallback for IBB/S5B and ICE
  without native authentication. The session policy is a default, overridable
  per content before preparation. Each application chooses its own protection
  and explicitly configures its logical transport; ICE does not inspect other
  contents. JET-protected data needs no additional fingerprint authentication.
  An established BUNDLE association is reused only when compatible, never
  upgraded or renegotiated for a new content. DTLS fingerprint verification uses OMEMO,
  including direct IQ negotiation with peers advertising the direct feature.
  BUNDLE followers inherit authentication only from their existing connected
  association with matching local and remote fingerprints. No per-range
  OMEMO exchange or JET wrapper is needed in that case.
  Replacing native protection with a transport requiring JET is rejected
  until a fresh content negotiation can carry the JET envelope.
- `Jingle::JetOmemo` implements only the XEP-0396 `JET::KeyTransport` adapter in the format of
  Conversations and Dino:
  - for file-transfer contents it creates in a session that prefers or requires end-to-end
    encryption, it adds `<security xmlns='urn:xmpp:jingle:jet:0' name cipher type>` with an OMEMO
    envelope of the key for all devices of the peer, when the peer advertises
    `urn:xmpp:jingle:jet-omemo:0`; otherwise the file goes unprotected, or a session requiring
    encryption refuses it;
  - the profile follows the peer's capabilities like messages: OMEMO 2 when advertised, with an
    SCE envelope holding `<secret xmlns='urn:xmpp:jingle:jet-omemo:0' key iv/>` and AES-256-GCM (a
    Psi extension, see `xsf-proposals/`); otherwise legacy OMEMO with a key transport message and
    AES-128-GCM, as Conversations and Dino;
  - received envelopes are retained only by their parsed file-transfer content;
    they are decrypted during that content's preparation. An IQ result need not
    wait for the cryptographic job, but a transfer must.
  - Jingle's common protection policy recognizes JET-protected file content;
    `OmemoDtlsVerification` knows only how to protect and authenticate fingerprints.
  - adding another JET key-delivery method does not require a signaling
    filter or a duplicate file-transfer implementation.
- `FileTransfer::Application` holds only a `JET::Protection` and a
  `Jingle::Connection::Ptr`; it neither handles key/IV nor appends/strips a GCM
  tag. File sizes and checksums remain those of the plaintext. The protected
  connection decrypts with AES-CTR from IV‖2 and checks the tag by re-encrypting
  the plaintext with GCM (the QCA API needs the tag before GCM decryption).
  Authentication failures end the content with `security-error`; any bytes
  previously exposed are untrusted and must be discarded.
- `tests/jingle/jetconnection.cpp` checks stream and datagram wrappers,
  zero-length records, tampered and missing tags, and their transparent
  plaintext lengths. `tests/jingle/jetcipher.cpp` checks the cipher against QCA's GCM decoder with any chunking,
  tampering and truncation; `tests/jingle/omemojet.cpp` sends a file over IBB between two clients
  with OMEMO: ciphertext with the tag on the wire, the plain file at the end, a tampered block
  refused, preferred encryption falling back without peer support and required encryption sending
  nothing.

## Plan

1. **OMEMO primitives** (done). Public API on `OmemoEncryption` for single-element encryption and
   decryption in both profiles, with trust checks: an end-to-end session requires a trusted device
   under the user's trust settings. The profile is chosen per peer from its capabilities
   (`OmemoEncryption::preferredProtocolFor()`): OMEMO 2 when the peer advertises it, otherwise the
   legacy profile (Conversations and Dino today), otherwise no end-to-end protection. A client
   that supports OMEMO 2 is expected to accept it wherever it accepts the legacy `<encrypted/>`
   element. For JET the profile is also named by the `type` attribute of `<security/>`, which
   XEP-0396 ties to the OMEMO namespace.
2. **Session policy** (done in Iris; applications set it). `Jingle::Session` gets an "end-to-end required" flag. Each transport
   reports its protection: none, encrypted but unauthenticated (DTLS with a plain fingerprint),
   end-to-end. Transport selection and fallback skip transports that cannot reach end-to-end
   under the policy; a content that ends up without protection is rejected or removed with
   `security-error`, and the application shows why.
3. **OMEMO-verified DTLS** (done in Iris; Psi supplies its UI policy and shows the result). Compatible with Conversations and Dino: the device id in XEP-0353
   `<proceed/>`, encrypted fingerprints in `session-initiate`, `session-accept`, `content-add`,
   `content-accept` and `transport-replace` of ICE transports. Iris also applies it to `ice:0`
   transports and to SCTP data channels (file transfer, whiteboards), which share the DTLS
   association; Conversations and Dino only look at `ice-udp:1`.
4. **JET-OMEMO for files** (done in Iris, on any transport). Compatible format and whole-stream AES-128-GCM, with the
   received file kept unverified until the tag matches.
5. **SXE payload in messages** (done in Iris): `SXE::SxeOmemo` protects every `<sxe/>` with OMEMO 2
   and SCE when both sides advertise `urn:xmpp:sxe:0#omemo`; the SXE transport then carries
   `encryption='urn:xmpp:omemo:2'`, counts as protected for the policy, and drops unprotected
   payload. Tested in `tests/jingle/sxeomemo.cpp`.

## Proposals

The proposals to the XSF are in [xsf-proposals/](xsf-proposals/README.md): OMEMO-verified DTLS
fingerprints (`protoxep-omemo-dtls-verification.md`), the JET stream format and chunks
(`xep-0391-jet.md`), JET with OMEMO 2 (`xep-0396-jet-omemo2.md`) and protected SXE payload
(`xep-0284-sxe-encryption.md`).
