# Windows cryptographic provider: validation notes

The `qca-wincrypto` provider uses Windows CNG, Crypt32 and Schannel. It is a
general-purpose QCA provider with no production dependency on OpenSSL.
Validation was performed on Windows 11 x64 with MSVC 19.44 and Qt 6.12.0 on
October 28-29, 2026. Windows 10 is the supported minimum; earlier Windows 10
versions and Qt 5/MinGW still require their corresponding CI/platform checks.

## Capability boundaries

| Area | Implemented behavior |
| --- | --- |
| Randomness | System-preferred `BCryptGenRandom` |
| Hashes and MACs | MD5, SHA-1/256/384/512 and their HMAC variants; SHA-3 where provided by the running OS |
| Derivation | PBKDF2-SHA-1/256/384/512 and HKDF-SHA-256 |
| Symmetric encryption | AES-128/192/256 in ECB, CBC, CBC-PKCS7, CFB, OFB, CTR and GCM modes |
| Certificates | DER/PEM import, properties, RSA public-key extraction, explicit trust validation and limited self-signed certificate creation |
| Stream TLS | Client TLS 1.2 and OS-supported TLS 1.3, hostname validation, channel binding and shutdown |
| DTLS | Version 1.2, both roles, mutual certificates, MTU handling and bounded retransmission |
| DTLS-SRTP | Four protection profiles and directional master key/salt export |
| RSA | Generation, components, PKCS#1 v1.5/OAEP/raw encryption, PKCS#1/PSS signatures and DER/PEM I/O |

RSA generation supports 2048-8192 bits in 64-bit steps, exponent 65537. Imports
support 512-16384-bit two-prime keys subject to native CNG validation. Public
views own separate public CNG handles. RSA component constructors preserve a
null key when the provider rejects input. Hashed signatures stream without
retaining the message; raw PKCS#1 input is limited to one block. PKCS#1 v1.5
signatures support raw input and MD5/SHA-1/256/384/512; PSS supports the SHA
digests, matching MGF1, trailer 1 and explicit/default digest-sized salts.
Encryption supports v1.5, OAEP-SHA-1 and raw RSA, including empty padded input;
the legacy private-encrypt/public-decrypt operations support v1.5 and raw.
SSL-specific v1.5 padding and other signature descriptors fail closed.

Public export is SPKI and private export is PKCS#8; import also accepts PKCS#1.
Encrypted PKCS#8 uses PBES2/PBKDF2 with AES-128/192/256-CBC or 3DES-CBC.
QCA's existing default PBE selects 3DES; callers can explicitly select
`PBES2_AES256_SHA1`. Export follows the PBE enum's SHA-1 PRF, with 1,300,000
iterations, a random 16-byte salt and a random IV. Import supports SHA-1,
SHA-256, SHA-384 and SHA-512 PRFs, binary passphrases, at most 10,000,000 PRF
calls, 1024-byte salts and 1 MiB passwords. DER/PEM inputs are limited to
64/128 KiB. Parsing rejects noncanonical DER, trailing bytes, inconsistent
components/CRT, restricted RSA-PSS keys and multi-prime RSA. Single DES and
traditional encrypted PEM are not supported.

Stream TLS server mode and stream TLS client identities, DTLS 1.0/1.3,
renegotiation, session import, DSA/DH/EC/Ed25519, extraction of non-RSA public
keys, CSR, CMS and PKCS#12 remain unsupported. Provider feature names do not imply
complete parity with another backend or support for every related QCA operation.

## Sensitive memory and failure handling

AES key objects and hash/HMAC state use uniquely owned, aligned buffers from
the same protected QCA allocator used by `SecureArray`. CNG retains pointers
into these buffers: they must not move or share storage while their handles are
alive. Cloned contexts receive separate buffers. Native handles are destroyed
before the associated object storage is wiped and released.

Cipher keys and mode state are released at finalization or failure. Completed
HMACs no longer recreate an unused keyed context or retain an extra key
reference; QCA supplies its saved key on the next `clear()`/`setup()`. HKDF
intermediates stay in `SecureArray`. Temporary byte vectors use a wiping
allocator, including when old allocations are discarded during growth.
Private RSA DER, components, decrypted keys and PBE intermediates also stay in
protected buffers. RSA private operations use CNG handles. The public QCA PEM
API necessarily returns a caller-owned `QString`; its plaintext lifetime is the
caller's responsibility. No plaintext `QByteArray`/base64 staging copy is made
inside the provider's private-key PEM codec.

Native failures are checked instead of returning a truncated digest or partial
cipher result. Failed cipher operations become terminal. PKCS#7 verification
examines all 16 bytes of the last block. GCM never publishes decrypted plaintext
before tag verification. Invalid lengths, derivation parameters, integer
overflows and empty authentication tags are rejected.

This storage contract does not control caller-owned copies or allocations
inside Windows/Schannel. The existing QCA allocator's page locking is best
effort: these changes do not establish a strict `VirtualLock`/no-swap guarantee.

## Bulk processing

GCM with a 96-bit nonce and a 12-16-byte tag uses native CNG, including
accelerated GHASH. Encryption streams complete blocks and retains at most a
partial block. Decryption retains ciphertext until authentication succeeds.
Other GCM parameters use fixed-iteration portable GHASH without secret-indexed
tables; this fallback is slower.

GCM limits a message to `2^39 - 256` bits. Its authenticated-decryption buffer
must also fit QCA's signed-int API. Larger data needs separately authenticated
records with distinct nonces at the caller's protocol layer.

CBC encryption uses native chaining. CBC decryption batches AES block calls;
CTR batches up to 64 counters per CNG call. ECB/CBC do not repeatedly copy an
entire input into a pending buffer. TLS walks bulk record input linearly rather
than copying the complete remaining tail after each record. DTLS processes
bounded datagrams and preserves application packet boundaries.

Algorithm object/digest lengths are cached and HKDF reuses its expansion
buffer. MSVC x86/x64 secure erasure uses a non-elidable store intrinsic with a
portable volatile fallback. Tests cover exact wipe ranges and unaligned buffers.

The following paired Release measurements were recorded during memory
hardening, using the median of seven series on the validation machine:

| Operation | Before | After |
| --- | ---: | ---: |
| AES-256-GCM, 32 bytes | 14.95 us | 14.72 us |
| HMAC-SHA-256, 32 bytes | 6.02 us | 5.84 us |
| HKDF-SHA-256, 42 output bytes | 6.96 us | 6.77 us |
| HKDF-SHA-256, 8160 output bytes | 92.85 us | 83.12 us |
| AES-256-GCM, 8 MiB | 8.67 ms | 8.88 ms |
| AES-256-CBC, 8 MiB | 18.16 ms | 15.38 ms |
| AES-256-CTR, 8 MiB | 18.07 ms | 14.23 ms |

The large GCM measurement differs by about 2.3%. These local measurements with
changing CPU frequency do not establish a regression or guarantee zero overhead.
They are not DTLS handshake benchmarks.

## Certificate and peer authentication

TLS uses the trust collection supplied to QCA; an empty collection does not
inherit system trust. Callers requesting Windows roots must explicitly pass
`QCA::systemStore()`. Chain building does not perform synchronous AIA/root
downloads. Supplied CRLs are used; explicitly requested revocation checks fail
when the required information is unavailable. This is not online OCSP checking.

For a nonempty hostname, Windows SSL policy checks the peer name, including SAN
precedence over CN. QCA also permits an empty hostname when the caller applies
its own protocol-specific identity rules. That mode still validates the chain.
The caller must accept the certificate/name or fingerprint before sending data
or using exported key material.

TLS 1.3 post-handshake records, including session tickets, are processed without
losing record state. Fragmented headers/records, authenticated record failures,
channel binding and shutdown are covered by tests. Channel bindings include
`tls-unique`, `tls-server-end-point` and TLS 1.3 `tls-exporter` where applicable.

## DTLS and SRTP

DTLS 1.2 supports both handshake roles and mutual certificates. Handshake
messages are fragmented to the configured MTU and lost flights use bounded
exponential backoff. Tests cover lost ClientHello, HelloVerifyRequest and final
server Finished, reordered fragments and a 300-byte MTU. Old handshake records
are not treated as renegotiation after the association is established.

QCA does not pass an actual UDP sockaddr to providers. The caller owns peer
routing and must bind an association to the intended peer. Schannel receives
a stable random cookie namespace for each association.

Supported SRTP profiles are `SRTP_AES128_CM_HMAC_SHA1_80`,
`SRTP_AES128_CM_HMAC_SHA1_32`, `SRTP_AEAD_AES_128_GCM` and
`SRTP_AEAD_AES_256_GCM`. SSPI profile identifiers use network byte order, while
the array length uses host byte order. Independent interoperability tests caught
this distinction, which two native endpoints could otherwise agree on wrongly.

The RFC 5764 exporter uses `EXTRACTOR-dtls_srtp` without a context. Directional
keys and salts stay in `SecureArray`; the source Windows export buffer is wiped
before `FreeContextBuffer`. An unnegotiated profile exposes no key material.
SRTP/SRTCP packet protection belongs to a separate media implementation.

Altered records, replayed packets and forged unencrypted alerts do not publish
plaintext or close an established association. Application datagrams must fit
the configured MTU; the provider does not split one into multiple messages.

## Schannel local-identity storage limitation

The original RSA key is an unnamed CNG key in memory. Schannel's credential
signer cannot use that key directly on the tested system. The provider imports
a separate copy into the Windows user CNG key store under a unique process/UUID
name, without overwriting existing keys. Temporary PKCS#8 bytes use `SecureArray`.

The named copy is deleted immediately after `AcquireCredentialsHandle` opens
its own reference, before the handshake proceeds. Deletion failure aborts setup;
RAII also cleans up intermediate failures. Tests check the current process's
namespace before session teardown and after a certificate/key mismatch.
Certificates stay in memory and are never installed in Windows trusted stores.

This is not a strict memory-only identity backend. Windows can briefly write
the copy to its protected key storage, and a process crash between import and
deletion can leave an orphan. Ordinary password-based client TLS and symmetric
operations do not use this path.

## Validation and reproduction

Use an MSVC developer environment with Qt in `CMAKE_PREFIX_PATH`:

```powershell
cmake -S . -B build/native -G Ninja -DBUILD_WITH_QT6=ON `
  -DBUILD_PLUGINS=wincrypto -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE `
  -DCMAKE_INSTALL_PREFIX="$PWD/build/native-sdk"
cmake --build build/native --parallel 4
$env:QCA_PLUGIN_PATH = "$PWD/build/native/lib/qca3-qt6"
ctest --test-dir build/native --output-on-failure `
  -R '^(WinCrypto.*|DTLSSRTP|Hashing|SymmetricCipher|MessageAuthenticationCode|KeyDerivationFunction)$'
cmake --install build/native
```

Add `-DBUILD_SHARED_LIBS=OFF` for static QCA. CI covers both configurations with
OpenSSL discovery explicitly disabled. The production DLL's imports contain
QCA, Qt, Windows crypto/security libraries and the C++ runtime, without libssl
or libcrypto.

`WinCrypto`, `WinCryptoTLS`, `WinCryptoDTLS` and `WinCryptoRSA` reported
82, 15, 15 and 12 passing cases respectively, including parameterized rows.
The suites also passed with
MSVC AddressSanitizer. The cipher/hash/MAC/KDF suites reported 486/69/9/26 passing
cases. The generic DTLS-SRTP suite reported 7 passed and 3 skipped; the skipped
cases explicitly require a different provider. A separate full QCA run found
an unrelated FileWatch timeout; that code was not changed.

`WinCryptoDTLS` also covers a 4 MiB burst of 4096 datagrams, authenticated packet
reordering/replay, bad MACs, forged alerts, shutdown, MTU limits, trust failures,
cipher/SSF constraints and temporary-key cleanup. Provider-specific tests select
`qca-wincrypto` explicitly so another backend cannot satisfy them accidentally.

In a separate build, `-DWINCRYPTO_TEST_OPENSSL_INTEROP=ON` enables an independent
OpenSSL peer; leave OpenSSL discovery enabled for that test build. It reported
14 passed and no skips with each of OpenSSL 3.5.0 and 4.0.1, covering both DTLS
roles, data-only traffic and exact keys/salts for all four SRTP profiles, plus
bidirectional RSA encryption, PKCS#1/PSS signatures and PKCS#8 interoperability
with all four supported PBKDF2 PRFs.
Only that test executable links OpenSSL. It is also enabled in the Windows
Qt 5/MinGW CI configuration that already supplies OpenSSL.

References: [RFC 6347](https://www.rfc-editor.org/rfc/rfc6347.html),
[RFC 5764](https://www.rfc-editor.org/rfc/rfc5764.html),
[RFC 7714](https://www.rfc-editor.org/rfc/rfc7714.html),
[SSPI SRTP profile parameters](https://learn.microsoft.com/en-us/windows/win32/api/sspi/ns-sspi-sec_srtp_protection_profiles),
and [Schannel ephemeral-key limitations](https://learn.microsoft.com/en-us/dotnet/core/extensions/sslstream-troubleshooting#handshake-failed-with-ephemeral-keys).
