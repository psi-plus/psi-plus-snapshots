// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-info.h"
#include <QDomDocument>

namespace XMPP::Jingle::RTP {
namespace {
    const char *tag(SessionInfo::Kind kind)
    {
        switch (kind) {
        case SessionInfo::Kind::Active:
            return "active";
        case SessionInfo::Kind::Hold:
            return "hold";
        case SessionInfo::Kind::Unhold:
            return "unhold";
        case SessionInfo::Kind::Mute:
            return "mute";
        case SessionInfo::Kind::Unmute:
            return "unmute";
        case SessionInfo::Kind::Ringing:
            return "ringing";
        }
        return nullptr;
    }
    bool mute(SessionInfo::Kind kind) { return kind == SessionInfo::Kind::Mute || kind == SessionInfo::Kind::Unmute; }
}
QString                    SessionInfo::ns() { return QStringLiteral("urn:xmpp:jingle:apps:rtp:info:1"); }
std::optional<SessionInfo> SessionInfo::fromXml(const QDomElement &xml)
{
    if (xml.namespaceURI() != ns() || !xml.firstChildElement().isNull() || !xml.text().trimmed().isEmpty())
        return {};
    std::optional<Kind> kind;
    for (auto candidate : { Kind::Active, Kind::Hold, Kind::Unhold, Kind::Mute, Kind::Unmute, Kind::Ringing })
        if (xml.localName() == QLatin1String(tag(candidate)))
            kind = candidate;
    if (!kind)
        return {};
    SessionInfo result;
    result.kind = *kind;
    if (mute(*kind)) {
        result.creator = ContentBase::creatorAttr(xml);
        if (result.creator == Origin::None)
            return {};
        if (xml.hasAttribute("name")) {
            result.name = xml.attribute("name");
            if (result.name.isEmpty())
                return {};
        }
    } else if (xml.hasAttribute("creator") || xml.hasAttribute("name")) {
        return {};
    }
    return result;
}
QDomElement SessionInfo::toXml(QDomDocument &doc) const
{
    if (!tag(kind))
        return {};
    auto xml = doc.createElementNS(ns(), QLatin1String(tag(kind)));
    if (mute(kind)) {
        if (!ContentBase::setCreatorAttr(xml, creator))
            return {};
        if (!name.isEmpty())
            xml.setAttribute("name", name);
    } else if (creator != Origin::None || !name.isEmpty()) {
        return {};
    }
    return xml;
}
}
