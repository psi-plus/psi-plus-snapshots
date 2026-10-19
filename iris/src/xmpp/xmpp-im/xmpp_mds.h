/*
 * xmpp_mds.h - XEP-0490 Message Displayed Synchronization wire helpers
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef XMPP_MDS_H
#define XMPP_MDS_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/xmpp_message.h>
#include <iris/xmpp-im/xmpp_pubsub.h>

class QDomDocument;

namespace XMPP {

/**
 * XEP-0490 wire codec. Publication and catch-up use the existing
 * PubSubManager (publish()/items()); this class owns no cursor state.
 *
 * Applications MUST verify pubsub#publish-options in account disco before
 * publishing, use publishOptions(), and accept notifications only from the
 * account's bare JID. They must also resolve the stanza ID in local history
 * and move their displayed cursor only forward. For MUC chats, expectedBy is
 * the room bare JID; for 1:1 chats, it is the account bare JID.
 */
class IRIS_EXPORT MessageDisplayedSync {
public:
    struct State {
        Jid               chat;
        Message::StanzaId stanzaId;
    };

    static QString       node();
    static PubSubOptions publishOptions();

    // Returns an empty element if the assigning JID/ID is invalid.
    static QDomElement payload(QDomDocument &document, const Message::StanzaId &stanzaId);

    // Parses one PEP item, checking the expected stanza-ID issuer. The caller
    // determines expectedBy from its conversation type, not the XML itself.
    static bool parseItem(const PubSubItem &item, const Jid &expectedBy, State *state);

    // Validate the source of a PubSubManager::eventReceived notification.
    static bool isOwnNotification(const Jid &sender, const Jid &account, const PubSubEvent &event);
};

} // namespace XMPP

#endif // XMPP_MDS_H
