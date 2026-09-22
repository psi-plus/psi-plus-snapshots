# Direct SRTP ownership

This branch implements the psimedia part of the cross-repository SRTP ownership migration tracked
by psi-im/psi#974 and `doc/jingle-srtp-migration-plan.md` in the matching Psi branch.

psimedia scope:

- add an optional secure-RTP interface without changing the existing Provider/RtpSessionContext
  1.6 vtable in place;
- report SRTP profiles supported by the actual backend;
- own one secure association per authenticated DTLS association/BUNDLE group;
- implement RTP/SRTCP protect/unprotect directly with libSRTP;
- keep GStreamer on plain RTP/RTCP through the existing RtpSessionBridge boundary;
- support multiple SSRCs and group-level ingress, including compound/shared RTCP;
- provide explicit epoch invalidation so old queued packets cannot cross a rekey/replacement;
- use the build-selected libSRTP crypto backend directly rather than adding QCA crypto hooks;
- do not depend on GStreamer's srtpenc/srtpdec plugin.

The secure interface is intentionally backend-neutral: no Iris, XMPP or Jingle types enter the
psimedia ABI. The detailed cross-repository sequencing and completion gates live in the Psi PR.
