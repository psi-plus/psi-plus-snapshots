// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_INFO_H
#define JINGLE_RTP_INFO_H
#include "jingle.h"
#include <QMetaType>

namespace XMPP::Jingle::RTP {
struct IRIS_EXPORT SessionInfo {
    enum class Kind { Active, Hold, Unhold, Mute, Unmute, Ringing };
    Kind kind = Kind::Active;
    // Only mute/unmute have creator and optional content name.
    Origin                            creator = Origin::None;
    QString                           name;
    static QString                    ns();
    static std::optional<SessionInfo> fromXml(const QDomElement &);
    QDomElement                       toXml(QDomDocument &) const;
};
}
Q_DECLARE_METATYPE(XMPP::Jingle::RTP::SessionInfo)
#endif
