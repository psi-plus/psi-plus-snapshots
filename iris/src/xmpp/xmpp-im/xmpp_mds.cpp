/*
 * xmpp_mds.cpp - XEP-0490 Message Displayed Synchronization wire helpers
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "xmpp_mds.h"
#include "xmpp_xmlcommon.h"

#include <QDomDocument>

namespace XMPP {
namespace {
constexpr auto MdsNs = "urn:xmpp:mds:displayed:0";
constexpr auto SidNs = "urn:xmpp:sid:0";

} // namespace

QString MessageDisplayedSync::node()
{
    return QLatin1String(MdsNs);
}

PubSubOptions MessageDisplayedSync::publishOptions()
{
    return {
        { QStringLiteral("pubsub#persist_items"), { QStringLiteral("true") } },
        { QStringLiteral("pubsub#max_items"), { QStringLiteral("max") } },
        { QStringLiteral("pubsub#send_last_published_item"), { QStringLiteral("never") } },
        { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } }
    };
}

QDomElement MessageDisplayedSync::payload(QDomDocument &document, const Message::StanzaId &stanzaId)
{
    if (!stanzaId.isValid())
        return {};

    auto displayed = XMLHelper::emptyTagNS(&document, QLatin1String(MdsNs), QStringLiteral("displayed"));
    auto sid = XMLHelper::emptyTagNS(&document, QLatin1String(SidNs), QStringLiteral("stanza-id"));
    sid.setAttribute(QStringLiteral("by"), stanzaId.by.bare());
    sid.setAttribute(QStringLiteral("id"), stanzaId.id);
    displayed.appendChild(sid);
    return displayed;
}

bool MessageDisplayedSync::parseItem(const PubSubItem &item, const Jid &expectedBy, State *state)
{
    if (!state || !expectedBy.isBare())
        return false;

    const Jid chat(item.id());
    if (!chat.isValid() || chat.isEmpty() || chat.full() != item.id())
        return false;

    const auto displayed = item.payload();
    if (!isElementNS(displayed, QLatin1String(MdsNs), QStringLiteral("displayed"))
        || displayed.hasAttribute(QStringLiteral("id")))
        return false;

    const auto sid = displayed.firstChildElement();
    if (!isElementNS(sid, QLatin1String(SidNs), QStringLiteral("stanza-id"))
        || !sid.nextSiblingElement().isNull())
        return false;

    const Message::StanzaId stanzaId { Jid(sid.attribute(QStringLiteral("by"))),
                                       sid.attribute(QStringLiteral("id")) };
    if (!stanzaId.isValid() || stanzaId.by != expectedBy)
        return false;

    State parsed;
    parsed.chat = chat;
    parsed.stanzaId = stanzaId;
    *state = parsed;
    return true;
}

bool MessageDisplayedSync::isOwnNotification(const Jid &sender, const Jid &account, const PubSubEvent &event)
{
    return sender.isBare() && account.isValid() && sender.compare(account, false)
        && event.type() == PubSubEvent::Type::Items
        && event.node() == node();
}

} // namespace XMPP
