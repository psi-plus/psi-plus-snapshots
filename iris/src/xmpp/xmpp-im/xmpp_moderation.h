/*
 * xmpp_moderation.h - XEP-0425 MUC moderated message retraction requests
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef XMPP_MODERATION_H
#define XMPP_MODERATION_H

#include <iris/jid/jid.h>
#include <iris/xmpp-im/xmpp_message.h>
#include <iris/xmpp-im/xmpp_task.h>

namespace XMPP {

/**
 * Sends one XEP-0425 retraction request to a MUC bare JID.
 * The target ID MUST have been issued by the room (XEP-0359).
 * The application MUST discover urn:xmpp:message-moderate:1 at the room
 * and establish that the target was a user chat message, not a non-messaging
 * payload. Only the MUC decides whether moderation is authorized.
 *
 * This is an IQ Task, not a MAM query. The resulting MUC moderation event
 * is distinct from the IQ result and requires separate processing.
 */
class IRIS_EXPORT ModeratedRetractionTask : public Task {
    Q_OBJECT
public:
    explicit ModeratedRetractionTask(Task *parent);

    void request(const Jid &room, const Message::StanzaId &target, const QString &reason = {});

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    Jid     room_;
    QString targetId_;
    QString reason_;
    bool    valid_ = false;
    bool    started_ = false;
    bool    awaitingResponse_ = false;
};

} // namespace XMPP

#endif // XMPP_MODERATION_H
