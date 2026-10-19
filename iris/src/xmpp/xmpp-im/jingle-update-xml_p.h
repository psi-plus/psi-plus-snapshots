// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_UPDATE_XML_P_H
#define JINGLE_UPDATE_XML_P_H

#include "jingle.h"

#include <QDomElement>
#include <QList>

namespace XMPP { namespace Jingle {

// XEP-0166 permits at most one <reason/> after all <content/> siblings.
// Application::takeOutgoingUpdate() provides a reason with each removal; a
// batched removal must serialize only one of those reasons on the whole IQ.
// Keep the first reason, while the caller logs every application's own reason.
inline int appendJingleActionUpdates(QDomElement &jingle, Action action, const QList<QDomElement> &updates)
{
    if (action != Action::ContentRemove && action != Action::ContentReject) {
        for (const auto &element : updates)
            jingle.appendChild(element);
        return 0;
    }

    QDomElement reason;
    int         discardedReasons = 0;
    for (const auto &element : updates) {
        const auto name = element.localName().isEmpty() ? element.tagName() : element.localName();
        if (element.namespaceURI() == NS && name == QLatin1String("reason")) {
            if (reason.isNull())
                reason = element;
            else
                ++discardedReasons;
        } else {
            jingle.appendChild(element);
        }
    }
    if (!reason.isNull())
        jingle.appendChild(reason);
    return discardedReasons;
}

}} // namespace XMPP::Jingle
#endif // JINGLE_UPDATE_XML_P_H
