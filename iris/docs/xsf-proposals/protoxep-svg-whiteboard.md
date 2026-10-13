# protoXEP: SVG whiteboarding over SXE — profile changes

Proposals for the minimal SVG whiteboard profile (`urn:xmpp:jingle:apps:svg-whiteboard:0`,
<https://github.com/jabberworld/whiteboarding>, `docs/xep-svg-whiteboarding-minimal.xml`,
`docs/WHITEBOARD_PROFILE.md`), to be discussed with its author. Written against the profile as
read in 2026 and XEP-0284 0.1.4.

1. **Multi-user whiteboards.** Jingle with a MUC room as the peer does not work: IQs to the
   room's bare JID go to the MUC service. A MUJI-like model (XEP-0272) fits better: advertise the
   session in room presence (XEP-0284 already defines `<sxe session name/>` in presence), send
   payload as `groupchat` messages, and let the initiator or any synchronized participant answer
   `<connect/>` with a state offer.
2. **Hosts.** `<host/>` in one-to-one examples should be the initiator's full JID.
3. **History versions.** Allow history `<set/>` chunks with the same version (character data split
   across edits), so a joiner keeps the host's record versions.
4. **Hints.** Recommend XEP-0334 `<no-store/>` (and `<no-copy/>`) for SXE messages.
5. **Connections.** Reference SXE payload on Jingle connections, see
   [xep-0284-sxe-connections.md](xep-0284-sxe-connections.md).
6. **Proposals.** Recommend XEP-0353 for proposing a whiteboard to all resources, with the
   description element of the profile in `<propose/>`.
7. **Senders.** Define Jingle `senders` of an SXE content as the participants allowed to edit:
   outside `senders` no `<new/>`, `<set/>`, `<remove/>` is sent and received edits are ignored;
   `none` freezes the board for everybody; `content-modify` changes it without a new state
   transfer.
8. **Protection.** Reference end-to-end protected payload, see
   [xep-0284-sxe-encryption.md](xep-0284-sxe-encryption.md).

Iris implements 2–8 for one-to-one sessions; see [../jingle-sxe.md](../jingle-sxe.md).
