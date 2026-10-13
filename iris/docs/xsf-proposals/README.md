# Proposals to the XSF

Changes to XMPP specifications that Iris and Psi implement or need, one file per specification
(XEP number or protoXEP). Each file names the version of the specification it was written
against, the problem, the proposed wire format, compatibility, and what Iris implements. They
are drafts for discussion with the authors and the XSF; nothing here has been submitted yet.

| File | Specification | Summary | Iris |
| --- | --- | --- | --- |
| [xep-0391-jet.md](xep-0391-jet.md) | XEP-0391 Jingle Encrypted Transports | Define the stream cipher (tag appended, sizes, hashes), errors, and authenticated chunks for resumption | Whole-stream AES-GCM as deployed; chunks not yet |
| [xep-0396-jet-omemo2.md](xep-0396-jet-omemo2.md) | XEP-0396 JET-OMEMO | An envelope for OMEMO 2, which has no key transport message | Implemented as a Psi extension |
| [protoxep-omemo-dtls-verification.md](protoxep-omemo-dtls-verification.md) | protoXEP (Conversations' draft namespace) | OMEMO-verified DTLS fingerprints in Jingle | Implemented, compatible with Conversations and Dino |
| [xep-0284-sxe-connections.md](xep-0284-sxe-connections.md) | XEP-0284 Shared XML Editing | SXE payload on reliable message-oriented Jingle connections | Implemented |
| [xep-0284-sxe-encryption.md](xep-0284-sxe-encryption.md) | XEP-0284 Shared XML Editing | End-to-end protected SXE payload in messages (OMEMO 2, SCE) | Implemented as a Psi extension |
| [protoxep-svg-whiteboard.md](protoxep-svg-whiteboard.md) | SVG whiteboard profile (protoXEP draft) | Changes to the whiteboard profile: MUC, hosts, versions, hints, proposals, senders | Partly implemented |

Related design notes: [../jingle-e2e.md](../jingle-e2e.md) and [../jingle-sxe.md](../jingle-sxe.md).
