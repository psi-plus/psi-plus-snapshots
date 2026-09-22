# SRTP migration to psimedia

This branch implements the Iris part of the cross-repository SRTP ownership migration tracked by
psi-im/psi#974 and `doc/jingle-srtp-migration-plan.md` in the matching Psi branch.

Iris scope:

- keep Jingle, ICE, BUNDLE and DTLS ownership;
- keep QCA fingerprint verification, DTLS-SRTP profile negotiation and key export;
- expose backend-neutral authenticated SRTP keying material plus security epoch/invalidation;
- pass protected SRTP/SRTCP datagrams between ICE and the media backend;
- preserve one association identity for all BUNDLE members sharing a DTLS association;
- move secure-profile capability selection to the intersection of QCA and the installed media backend;
- remove Iris packet protect/unprotect and the direct libSRTP dependency after the new media path is wired;
- preserve make-before-break transport replacement and reject stale security epochs.

The final design must not create one crypto context per RTP content. The media boundary needs
association/group-level ingress so compound RTCP spanning BUNDLE members can be handled without
broadcasting or intentional drops.

This document is a branch-local implementation marker; the detailed cross-repository execution
plan lives in the Psi PR.
