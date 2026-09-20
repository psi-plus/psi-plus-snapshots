# Native Jingle calls interoperability status

This document records verified call-stack baselines and interoperability results. It intentionally distinguishes source-level support, synthetic tests, real packet-path tests and calls against external peers.

## Current integration snapshot — 2026-09-20

Two independent evidence tracks are current and must not be conflated:

1. **Server-mediated native audio is verified.** Psi integration run `35475382811`
   (`PsiMedia integration #87`), job `105983625438` (`xmpp-prosody-call`) passed on
   `ci/psimedia-integration` commit `4537a70cd50ed1a9db4d85fbae24ab3458e5f030`.
   The job explicitly checked out Iris `8af85f489dc1cc196c7b95c1fa58f3f4a09ae4cd`
   from `jingle/async-media` and psimedia
   `2d067da46a70a74b1ccf91830c97b09a8c58713b` from `jingle/rtcp-session`.
2. **Production negotiated BUNDLE wiring is verified by Iris regressions.** Iris
   `9985f52d573176a7b3be9c40fdbf6911a3f4e7f1` passed `Jingle regressions #269`
   (run `35515002546`): qca3-srtp **58/58 passed**, including
   `jingle_bundleice`, `jingle_bundlemedia` and `jingle_bundlesignaling`; the
   transport sanitizer job also passed.

### Server-mediated Psi ↔ Psi audio

The Prosody gate runs two separate real client processes/accounts through a real local XMPP
server using the production psimedia/GStreamer backend. The observed call used
`urn:xmpp:jingle:transports:ice-udp:1`, DTLS/SRTP and Opus/48 kHz.

Both caller and callee reported:

- `CALL_MEDIA_STARTED=1`;
- one negotiated local and remote payload;
- `CALL_DECODED_BYTES=622080`;
- successful terminal call result.

The output PCM files were 664320 bytes on each side. The logs contain real
`session-initiate`, `session-accept` and successful `session-terminate` signaling.
This closes the previous “Psi ↔ Psi native audio through a server” blocker for the tested
headless CI topology. It does not establish WAN/NAT/TURN behavior, device capture behavior or
Conversations interoperability.

### Negotiated BUNDLE source/runtime regression evidence

The current Iris RTP path no longer uses one ICE association per content when BUNDLE is
negotiated. Per-content `Transport` objects retain signaling identity, while the session-local
ICE Pad commits a shared `IceConnection`/DTLS/SRTP association through
`ConnectionRegistry` and `ConnectionGroupTransaction`. RTP packet ingress is routed through
the Pad-owned `BundleRouter`.

The signaling regression exercises the production parser/scheduler boundary rather than a
test-only group mutator:

- real `session-initiate` serialization and IQ result;
- real `session-accept` XML with negotiated audio+video BUNDLE;
- partial BUNDLE `transport-replace` rejection without mutation;
- full-group replacement with fresh ICE transports;
- production `prepareTransport()` for both replacements;
- one live BUNDLE association after atomic switch;
- one shared replacement SRTP session;
- stale SRTP callbacks from the retired transport generation cannot terminate the replacement.

This is strong source/local-runtime evidence for negotiated BUNDLE and atomic pre-Connecting
replacement. It is **not an external live BUNDLE interoperability result**. In particular,
group-level `SharedRtcp` delivery is recognized by `BundleRouter` but still has no psimedia
group ingress and is intentionally dropped rather than duplicated across contents. Active-call
transport replacement remains disabled from `Connecting` onward pending explicit media/SRTP
migration semantics.

### Current result matrix

| Scenario | Result | Evidence / remaining boundary |
| --- | --- | --- |
| Production psimedia plugin and RTP bridge | pass | cross-repo integration gates |
| Psi ↔ Psi native audio through Prosody | **pass** | run `35475382811`, job `105983625438`; 622080 decoded bytes per side |
| ICE-UDP:1 signaling/packet path | pass | live audio gate plus standalone Jingle ICE-UDP regressions |
| Negotiated audio+video BUNDLE in Iris | **pass in local regression** | `Jingle regressions #269`; shared ICE/DTLS/SRTP and atomic full-group replacement |
| External live audio+video BUNDLE | **not claimed** | no pinned external peer/live A/V gate; SharedRtcp group media ingress and active migration remain open |
| Psi ↔ Conversations audio | **blocked — peer not pinned** | exact peer/device/server/TURN metadata and sanitized fixture still missing |
| TURN relay-only call | **not claimed** | no call-level relay-only evidence |

Conversations remains unpinned: there is no recorded release/commit, Android device,
server/TURN configuration or authorized sanitized call capture. Do not infer its caps,
BUNDLE, JMI or transport-replace behavior from Psi/Psi tests.

The capability-selection negative matrix is also verified on Iris
`b632ff84fbcb6d8cef008c75819fb89dd29cef79`: `Jingle regressions #271` passed both
qca3-srtp and transport-sanitizer jobs. Current RTP policy is fail-closed: advertising RTP/audio
plus only IBB or S5B transport capability does not make those byte-stream transports valid for
the packet-oriented RTP backend; advertising grouping alongside IBB does not change that result.
A future low-bitrate codec does not change this contract without an explicitly implemented RTP
transport/profile.

## Earlier audit snapshot — 2026-09-13

Reviewed ranges:

- Psi `9421bd0e` → `c8821dbe`.
- Iris `ba784334` → `be3833e`.
- psimedia `ab15f68` → `b4139cbd`.

Local Iris build used one `-j2` job, Qt 6.10.2 / QCA3 3.0.3 with SRTP and SCTP.
Serial CTest with loopback socket permission: **24/24 passed**. This includes the new
content-modify, content-modify-race and RTP direction tests. The standalone Psi
`src/avcall/unittest/policy.cpp` was compiled and passed separately; this is not a full Psi build.

An extra temporary reproducer using the actual Iris Application/ACK code and Psi policy
helper confirms two uncovered scenarios: crossed requests leave divergent senders, and a
failed request is discarded by Iris while the client policy still suppresses the same target.
This is application/helper-level evidence plus dispatcher source review, not a live two-peer
XMPP run. Permanent two-Session/controller regressions remain required.

psimedia runtime tests were not run locally: `gstreamer-app-1.0` development pkg-config
dependency is missing. New remote CI runs, sanitizer runs and live calls were not verified
in this audit. Source review confirms production bridge wiring and live-ID rebuild, but also
the exclusion of live/file transitions and the reset of receive/video alongside mic changes.
The audio-only sender test is not evidence of physical capture release or A/V continuity.

See [the development plan](jingle-calls-implementation-plan.md#2-новый-audit-gate-исправления-доказательства-и-оставшиеся-проблемы)
for corrective actions and required regression scenarios, and
[native RTP architecture](jingle-rtp-design.md#psi-and-psimedia-production-boundary)
for the current production packet path. No Conversations parity or live BUNDLE is claimed.

## Earlier Iris snapshot — 36f9e3a

Reviewed changes from `7c0acb0c7dc45a9173faba5521f551916e9d67d7` to
`36f9e3ac5e326633b56cd24c2d2c8dea4b638732`: guarded initial-subset cleanup,
empty initial-answer rejection and runtime SSRC retention across router reconfiguration.
Psi and psimedia were not re-audited for this snapshot.

Local verification: Qt 6.10.2, system QCA3 3.0.3, SRTP and SCTP enabled;
one build with `-j2`, serial CTest: **21/21 passed**. Loopback tests require socket
permissions; the sandbox-only attempt could not bind UDP, while the permitted run passed.
This includes both `ice:0` and `ice-udp:1` local packet paths, not external peers.

The working-tree acceptance regressions additionally cover removal before queued start,
self/sibling deletion from start callbacks, empty-session termination, Session cancellation
and destruction, and later content acceptance without duplicate activation.
With this working-tree fix, the full serial Jingle suite passes **21/21** in the same environment.
See [scheduling semantics](jingle.md#session-state-versus-connectivity).

Standard ICE-UDP, native RTP and asynchronous media interfaces are implemented.
Group/membership and BundleRouter components have standalone tests, but are not wired
into a production shared association. No live BUNDLE or Conversations result is claimed.
See [native RTP architecture](jingle-rtp-design.md) for the current layer boundaries.

The dated baseline below is retained as a record of that run, not a description of
current source capabilities. Its absent Psi adapter is not a current Iris finding.

## Historical baseline — 2026-09-11

Iris baseline commit: `874a3a6b2a3d29d4ad3be3a1d848fd14dad0e7de` (`docs/jingle-architecture`).

The pull-request CI run `Build and test #267` completed successfully. The regular CI matrix builds Qt 5/QCA 2, Qt 6/QCA 2, Qt 6/QCA 3 and bundled-QCA configurations, plus desktop and Android variants. `IRIS_ENABLE_SRTP` is OFF by default and the standalone `tests/jingle` project is not executed by that workflow, so this green build is a build/install baseline, not an SRTP or call-interoperability result.

At this baseline, the standalone Jingle test project defines:

- `jingle_signaling`
- `jingle_dtlssrtp`
- `jingle_iceownership`
- `jingle_rtpdescription`
- `jingle_rtpnegotiation`
- `jingle_rtpapplication`
- `jingle_srtp`
- `jingle_transportacks`
- `jingle_icertp` and `jingle_rtpmedia` when QCA 3 and SRTP are enabled

The last two targets exercise real local UDP/ICE/DTLS/SRTP with mock application/media integration. They are not evidence of a call through an XMPP server or interoperability with Conversations.

### Baseline transport profile

The ICE Jingle manager supports both the legacy `urn:xmpp:jingle:transports:ice:0` profile and the Stable XEP-0176 `urn:xmpp:jingle:transports:ice-udp:1` namespace on the shared `Ice176` engine. DTLS is advertised only when runtime support is available. Packet-oriented RTP requires authenticated RTP/RTCP mux and uses the negotiated secure ICE transport; IBB/S5B are not implicit RTP fallbacks.

### External peer baseline

Conversations version/commit: **blocked — not pinned yet**.

Android version/device: **blocked — not recorded yet**.

Server and TURN configuration: **blocked — not recorded yet**.

Sanitized JMI/session-initiate/session-accept/transport-info fixtures: **blocked — no authorized test-call capture has been added yet**.

These unknowns must not be replaced with guessed peer behaviour. When a real test peer is available, record exact versions and add sanitized fixtures under `tests/jingle/fixtures/` before claiming Conversations parity.

### Baseline result matrix

No real end-to-end call result is claimed at this baseline.

| Scenario | Peer | Result | Evidence |
| --- | --- | --- | --- |
| Psi ↔ Psi native audio | not integrated yet | blocked | native Psi call controller/media adapter not implemented |
| Psi ↔ Conversations audio | Conversations not pinned | blocked | no captured fixture or real call |
| Audio + video / BUNDLE | no peer | blocked | shared association/router not implemented |
| TURN relay-only | no peer | blocked | no call-level test |

Update this table only with observed results and the exact commits/library versions used for the run.
