/*
 * xmpp_moderation.cpp - XEP-0425 MUC moderated message retraction requests
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "xmpp_moderation.h"

#include "xmpp_xmlcommon.h"

#include <QDomDocument>

namespace XMPP {

ModeratedRetractionTask::ModeratedRetractionTask(Task *parent) : Task(parent)
{
    // Task dispatch may still visit completed children before deleteLater().
    connect(this, &Task::finished, this, [this] { awaitingResponse_ = false; });
}

void ModeratedRetractionTask::request(const Jid &room, const Message::StanzaId &target,
                                     const QString &reason)
{
    room_ = room;
    targetId_ = target.id;
    reason_ = reason;
    valid_ = room.isValid() && !room.isEmpty() && room.resource().isEmpty()
        && target.by.isValid() && !target.by.isEmpty() && target.by.resource().isEmpty()
        && room == target.by && !targetId_.isEmpty();
}

void ModeratedRetractionTask::onGo()
{
    if (!valid_) {
        setError(1, QStringLiteral("Moderated retraction requires a MUC-issued stanza ID and bare room JID"));
        return;
    }

    static const QString moderateNs = QStringLiteral("urn:xmpp:message-moderate:1");

    auto iq = createIQ(doc(), QStringLiteral("set"), room_.bare(), id());
    auto moderate = doc()->createElementNS(moderateNs, QStringLiteral("moderate"));
    moderate.setAttribute(QStringLiteral("id"), targetId_);
    moderate.appendChild(doc()->createElementNS(QStringLiteral("urn:xmpp:message-retract:1"),
                                                QStringLiteral("retract")));
    if (!reason_.isEmpty())
        moderate.appendChild(textTagNS(doc(), moderateNs, QStringLiteral("reason"), reason_));
    iq.appendChild(moderate);
    awaitingResponse_ = true;
    send(iq);
}

bool ModeratedRetractionTask::take(const QDomElement &stanza)
{
    if (!awaitingResponse_ || !iqVerify(stanza, room_, id()))
        return false;

    // iqVerify() authenticates the IQ envelope and id, not the response
    // type: an incoming get/set can independently reuse the same id.
    const auto type = stanza.attribute(QStringLiteral("type"));
    if (type != QLatin1String("result") && type != QLatin1String("error"))
        return false;

    if (type == QLatin1String("result")) {
        setSuccess();
    } else if (stanza.firstChildElement(QStringLiteral("error")).isNull()) {
        setError(1, QStringLiteral("Moderation IQ error without error payload"));
    } else {
        setError(stanza);
    }
    return true;
}

} // namespace XMPP
