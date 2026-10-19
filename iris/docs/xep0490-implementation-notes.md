# XEP-0490 implementation prerequisites

This branch is work in progress. The existing PubSubManager handles publishing and fetching PEP items; no new manager or MAMTask extension is needed.

Before publication, verify account disco support for `pubsub#publish-options`. For incoming notifications, verify the sender is the account bare JID, resolve stanza IDs against local history, and never move the displayed cursor backward. MUC stanza IDs must be issued by the room bare JID.

This note does not claim that XEP-0490 is implemented.
