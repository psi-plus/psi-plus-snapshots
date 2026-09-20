# Jingle interoperability fixtures

This directory is reserved for sanitized protocol fixtures captured from authorized test sessions.

Each fixture added here must document:

- peer product and exact version/commit when known;
- Iris/Psi commit used for the capture;
- Android/desktop platform where relevant;
- server and TURN configuration relevant to the protocol flow;
- whether the XML is an exact capture or a minimized reproduction;
- which secrets or identifying values were replaced.

Do not commit real ICE/TURN passwords, DTLS/SRTP key material, private keys, account JIDs, public IP addresses or unrelated message content. Use documentation/example address ranges and obvious placeholder credentials when normalization is required.

Keep synthetic fixtures clearly labelled as synthetic. A synthetic XEP example is useful for parser tests but is not evidence of Conversations interoperability.

## Current status

As of 2026-09-19 no Conversations fixture has been committed. Remote CI now
contains synthetic one-real-backend packet-path coverage for production
psimedia plugin loading, device-free Opus/VP8 negotiation, independent
audio/video ICE + DTLS-SRTP transports, consent/stop, no-device receive-only
behaviour and terminal runtime-error cleanup. Those packets and descriptions
are generated inside the test process; they are not an authorized external
peer capture and must not be committed here as if they were interoperability
fixtures.

The exact Conversations release/commit, Android device, server/TURN
configuration and a sanitized authorized test-call capture remain external
prerequisites for the interoperability matrix in
`docs/jingle-calls-interop.md`. A real server-mediated Psi ↔ Psi call also
remains required before P1c acceptance.
