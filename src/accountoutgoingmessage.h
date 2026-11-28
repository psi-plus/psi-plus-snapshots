// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <iris/xmpp_message.h>

// Mediated MUC invitations are addressed to the room service, not to an
// encryption peer. The service must be able to read the MUC user extension.
// Groupchat payloads use their own explicit encryption path.
inline bool usesAutomaticContactEncryption(const XMPP::Message &message)
{
    return message.xencrypted().isEmpty() && message.type() != XMPP::Message::Type::Groupchat
        && message.mucInvites().isEmpty();
}
