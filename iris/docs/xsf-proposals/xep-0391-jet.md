# XEP-0391: Jingle Encrypted Transports — stream format, errors and chunks

Written against XEP-0391 version 0.1.2 (2018-07-31, Deferred).

## Problem

XEP-0391 says that file data is encrypted with the transport key and IV, but not how:

- whether the cipher runs over the whole stream or per chunk, and where the GCM tag goes;
- whether `size` and the hashes of XEP-0234 describe the plain or the encrypted file;
- what the responder answers when it cannot decrypt the envelope or does not support the cipher;
- how a ranged or resumed transfer is encrypted (section 4.3 only says a new secret is created);
- the cipher namespaces appear with (`urn:xmpp:ciphers:aes-128-gcm-nopadding:0`, table 2) and
  without the `:0` suffix (examples).

Deployed clients (Conversations, Dino) agree on one interpretation, which the specification
should record.

## Proposal

### 1. Record the deployed format (no wire change)

- The whole transfer is one AES-GCM stream over the plain file; the 16-byte tag is appended. The
  transport carries `size + 16` bytes.
- `size` and the `<hash/>` elements of the file description describe the plain file, as the
  checksum after the transfer does.
- The receiver keeps the last 16 bytes as the tag and treats the received data as unauthenticated
  until the tag matches; on a mismatch it discards the data and ends the content with
  `security-error`.
- Cipher namespaces are accepted with and without `:0`; senders write them without, as deployed.
- An envelope that cannot be decrypted, an unknown `type`, or an unsupported `cipher` is answered
  with the IQ error `not-acceptable` and, if the session continues, `content-reject`/
  `content-remove` with `security-error`. A content protected by JET MUST NOT fall back to an
  unprotected transfer.

### 2. Authenticated chunks (new cipher namespace, for resumption)

A whole-stream tag cannot be verified before the end, which is bad for large files and makes
resumption impossible without re-sending everything. A chunked variant, negotiated by a new
cipher namespace (for example `urn:xmpp:ciphers:aes-256-gcm-chunked:0`):

- the plain file is split into chunks of a fixed size `C` (signalled as a `chunk-size` attribute of
  `<security/>`, for example 65536);
- chunk `i` is encrypted with AES-GCM, nonce = IV XOR the 96-bit big-endian chunk number, and the
  additional authenticated data = the chunk number and a final-chunk flag; each encrypted chunk
  is followed by its 16-byte tag;
- a range request (`<range offset/>`) starts at a chunk boundary; the receiver verifies each chunk
  as it arrives and can resume from the last verified chunk with the same secret.

Psi works on resumable encrypted chunks in another project first (anykeep); the details here will
be revised from that experience before they are proposed.

## Compatibility

Part 1 describes what Conversations and Dino already send. Part 2 uses a new cipher namespace,
so peers without it keep the whole-stream format.

## Iris

`JET::StreamCipher` (`jingle-jet_p.h`) implements part 1 for AES-128 and AES-256; ranged and
streaming transfers with JET are refused for now. See [../jingle-e2e.md](../jingle-e2e.md).
