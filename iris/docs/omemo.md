# Encryption and OMEMO in Iris

## Architecture

Iris has one encryption registry, `XMPP::EncryptionManager`. Encryption protocols are
runtime `EncryptionMethod` objects and advertise one or more capabilities:

- `XmppStanza` — XMPP stanza encryption. OMEMO implements this capability.
- `DataMessage` — an independent byte message processed at once.
- `DataStream` — incremental encryption/decryption with explicit finalization
  and stream authentication.

`EncryptionManager` is the authoritative registry of encryption methods and a session
factory. It selects methods by the **entire** requested set of capabilities, reports
method-specific peer availability, and closes sessions when their methods are
unregistered. It does not execute cryptographic operations or own stream state.

`EncryptionMethod` implements one protocol (such as OMEMO) and advertises its
capabilities and availability. Its `startSession()` creates an `EncryptedSession` for
a specific use. A session can support multiple capabilities simultaneously, e.g.
protecting Jingle signaling and the stream tied to it.

`EncryptedSession` owns the requested operation capabilities, active jobs, session
closing, and stream state. Its public `encrypt()`, `decrypt()`, `encryptChunk()`,
and `decryptChunk()` methods validate and track operations, delegating cryptography
to protected virtual `doEncrypt*` and `doDecrypt*` implementations. Chunk calls
are sequential; a final successful decryption is required for authentication, and
intermediate decrypted bytes must not be exposed as verified content.

Network-dependent methods may return an unfinished `EncryptionJob` while they
fetch key material. This keeps XMPP `Task` as the network primitive and avoids
making cryptographic session objects own network state. Psi may adapt plugin-provided
methods into this Iris registry and expose UI controls; encryption protocol policy
and processing belong in Iris.

### Lifetime and peer identity

`Client` does not maintain a parallel JID/resource table. An application installs
`Client::setIncomingEncryptedStanzaHandler()`: Iris passes an encrypted stanza,
method ID, operation context and purpose to the application, which chooses or
creates its own `EncryptedSession` and returns the decryption job. The
application may reuse sessions attached to existing contacts and MUC occupants,
or create operation-local sessions for unknown senders and archive replay.
Unresolved requests fail closed; Iris never creates provisional contacts.

The application creates/reuses stanza sessions using
`EncryptionManager::startSession()` and closes them on the corresponding
contact/resource/operation lifecycle. `EncryptionManager` remains the method
registry and session factory, and closes all sessions for an unregistered
method; it is not a conversation or resource directory.

Jingle and other protocols with a known operation owner **pass their bound
`EncryptedSession` explicitly**, rather than looking it up by JID. Method-only
outgoing calls use an operation-local session and close it when no longer needed.
Incoming Jingle IQs reach `JTPush` through the existing `Task` dispatcher and
are routed by (peer, sid). JET security is attached to the selected file-transfer
content during `Session::parseContentAddList()` and decrypted during
`FileTransfer::Application::prepare()`, not by a separate signaling pre-filter.
DTLS fingerprint verification is performed by the selected ICE transport
after parsing the content and before using the peer fingerprint, via the
Jingle manager's optional `OmemoDtlsVerification` service. No DTLS filter
rescans the complete Jingle XML, and there is no global (peer, sid) cache
of pending DTLS authentication or JET keys.

A long-lived session's `context()` identifies its peer and defaults, but
`encrypt(stanza, operationContext)` and `decrypt(stanza, operationContext)`
merge a separate immutable context for **each** stanza. This is essential for
overlapping asynchronous work: replies (`replyTo`), MUC sender identity, and
forwarded carbons cannot mutate a shared session-wide context or leak into
subsequent messages. Method and plugin implementations receive the merged
context for that operation.

OMEMO's actual Double Ratchet sessions remain in the libsignal storage by
bare JID **and device ID** (separate for legacy and OMEMO 2). They survive
the lifetime of any application-owned stanza `EncryptedSession`, which must
neither replace nor reset those underlying ratchets.

OMEMO does **not** claim `DataMessage` or `DataStream`: using the Double Ratchet to
bulk-encrypt a file or media stream is the wrong layer. A file-encryption method (for
example an XEP-0454 implementation) can later implement `DataMessage`/`DataStream`,
while OMEMO transports/authenticates its key material in an encrypted stanza.

## XEP-0420

`StanzaContentEncryption` is independent of OMEMO. It splits a stanza into a
server-visible shell and an encrypted SCE envelope and, after decryption, restores
only the allowed application content. Server-processed elements received inside the
encrypted envelope are discarded. The random-padding fixed minimum is an
implementation profile setting; the XEP only fixes the extra random range.

## PubSub / PEP

`PubSubManager` provides reusable Iris tasks for item retrieval, publication,
retraction, node creation and node configuration. It also observes PEP event
messages without consuming them. OMEMO uses it for the device-list and bundle nodes;
application protocols such as AnyKeep synchronization can use exactly the same
manager.

## OMEMO implementation

`OmemoEncryption` is one OMEMO engine with two wire profiles:

- `OmemoProtocol::Omemo2` — `urn:xmpp:omemo:2`, SCE and the current OMEMO payload format.
- `OmemoProtocol::Legacy` — `eu.siacs.conversations.axolotl`, for interoperability with
  Psi's historical OMEMO plugin and other XEP-0384 0.3.x clients.

Both profiles share local identity keys, signed prekeys, one-time prekeys, trust policy and
high-level orchestration. They deliberately keep independent remote Double-Ratchet session
blobs because the Signal protocol generations on the wire are different. `libomemo-c` 0.5.1
is the X3DH/Double-Ratchet backend for both profiles.

OMEMO 2 is the baseline profile. Legacy support is enabled at runtime only when the active QCA
provider supplies AES-128-GCM; if it does not, Iris continues to advertise and use OMEMO 2 rather
than disabling the entire OMEMO method. `supportedProtocols()` exposes the local result.

All cryptographic primitives requested through libomemo-c's crypto-provider API and
all OMEMO payload cryptography go through QCA:

- cryptographically secure random bytes;
- SHA-512;
- HMAC-SHA-256;
- RFC 5869 HKDF-SHA-256 (built from QCA HMAC);
- AES-CBC with PKCS#7 padding;
- AES-CTR without padding;
- AES-128-GCM for the legacy OMEMO payload profile.

Iris itself does not call OpenSSL for OMEMO. The selected QCA provider may of course
be the QCA OpenSSL provider. Curve/XEdDSA/Double-Ratchet operations are internal to
libomemo-c; its symmetric/hash callbacks are backed by QCA.

`libomemo-c` is GPLv3, therefore OMEMO is an optional Iris build component. Generic
encryption, SCE, trust-storage and PubSub APIs remain available without it.

## Building

Enable OMEMO with:

```sh
cmake -S . -B build -DUSE_QT6=ON -DIRIS_ENABLE_OMEMO=ON
cmake --build build
```

By default Iris builds `protobuf-c` and `libomemo-c` as static external projects.
Use `-DIRIS_BUNDLED_OMEMO_C=OFF` to use a system `libomemo-c >= 0.5.1` found via
pkg-config. Local source trees can be supplied with `IRIS_OMEMO_C_SOURCE_DIR` and
`IRIS_PROTOBUF_C_SOURCE_DIR`.

The bundled dependency build forwards the active CMake toolchain/sysroot, Apple
architectures, Android ABI/API/NDK and Qt host paths. The bundled QCA build does the
same and always uses the `psi-im/qca` fork. On Android Iris selects the matching
per-ABI Qt/KDAB `android_openssl` bundle without consulting host pkg-config and
exports `iris_deploy_android_openssl(target...)` so an application can package the
matching runtime libraries. On iOS the old Homebrew OpenSSL fallback is disabled; a
cross-compiled OpenSSL root/provider must be supplied by the toolchain.

The build plumbing is therefore cross-compile-aware for Linux, Haiku, Windows,
macOS, Android and iOS, but Android/iOS still need real device/CI validation before
we call those platforms tested.

## Storage and trust

`OmemoStorage` is synchronous because libomemo-c's storage callbacks are synchronous.
A production implementation should use an in-memory authoritative state and persist
mutations atomically. `MemoryOmemoStorage` is only for tests/ephemeral clients.

Identity trust is intentionally separate in `EncryptionTrustStorage`. The Double
Ratchet layer rejects a changed identity key for an already known device, while the
application decides whether a newly learned fingerprint is automatically trusted,
manual, authenticated or distrusted.

For an AnyKeep-style explicit recovery flow, set new identities to undecided before
fetching/building sessions:

```cpp
omemo.setNewIdentityTrustLevel(XMPP::EncryptionTrustLevel::Undecided);
```

After the UI/recovery logic approves a fingerprint, call `setTrustLevel()` and retry
the operation or `refreshBundle()`.

## Basic use

```cpp
XMPP::MemoryOmemoStorage storage;              // replace with persistent storage
XMPP::MemoryEncryptionTrustStorage trust;      // replace with persistent trust DB
XMPP::OmemoEncryption omemo(client, &storage, &trust, client);

omemo.setNewIdentityTrustLevel(XMPP::EncryptionTrustLevel::Undecided);
auto setup = omemo.setUp(QStringLiteral("AnyKeep desktop"));
```

For OMEMO 2, arbitrary stanzas supported by SCE can be encrypted. When a concrete online
resource is known, pass its **full JID** in the encryption context. Iris uses its existing
XEP-0030/entity-caps cache to negotiate the wire profile for that resource and prefers OMEMO 2
when the resource advertises both profiles:

```cpp
XMPP::EncryptionContext context;
context.recipients = { peerFullJid };  // e.g. user@example.org/Psi+
auto job = client->sendEncrypted(stanza, XMPP::OmemoEncryption::methodId(), context);
```

Legacy OMEMO is message-only. A full JID advertising
`eu.siacs.conversations.axolotl.devicelist+notify` selects the legacy profile; a resource
advertising `urn:xmpp:omemo:2` or `urn:xmpp:omemo:2:devices+notify` selects OMEMO 2. The
selection is also recorded in `EncryptionMetadata::details["omemoProtocol"]`, so deferred
replies retain the same wire profile.

If no full resource is available, the generic API defaults to OMEMO 2. Applications that need
legacy negotiation should therefore resolve the target resource before encryption rather than
trying to infer capabilities from a bare JID. For an explicitly selected profile (tests,
recovery flows or protocol bridging), set `EncryptionContext::options["omemoProtocol"]` to
`"legacy"` or `"omemo2"`.

Incoming encrypted stanzas are decrypted before normal Iris task dispatch.
`Task::encryptionMetadata()` is valid while the decrypted stanza is being dispatched.
Copy it if a reply is deferred. For an encrypted IQ request this permits a response
to be routed back to the original sender device:

```cpp
auto metadata = *encryptionMetadata();
// ...later...
client->replyEncrypted(reply, metadata);
```

For MUC/group scenarios `EncryptionContext::recipients` must contain the real bare
JIDs whose devices are intended recipients; the stanza's `to` may remain the room JID.

## Protocol lifecycle handled by Iris

The OMEMO component itself handles:

- fetching/caching device lists and bundles;
- checking a newly generated local device id for collision before first publication;
- preserving other active own devices while publishing the device list;
- verifying signed device labels before exposing them;
- session construction only for identities accepted by trust policy;
- re-publishing a bundle after a one-time PreKey is consumed and replenished;
- empty OMEMO session-management messages and automatic key-exchange acknowledgements;
- the XEP-0384 ratchet-counter heartbeat rule (counter >= 53 on the first message for a ratchet key);
- re-announcing the local device when a concurrent PEP device-list update drops it;
- retrying OMEMO 2 bundle/device publication after repairing an existing PEP node's config;
- publishing both modern and legacy device-list/bundle representations from the same local key material;
- selecting legacy versus OMEMO 2 from cached disco features of the concrete full JID;
- keeping legacy and OMEMO 2 ratchet sessions independent while sharing identity/prekey lifecycle.
