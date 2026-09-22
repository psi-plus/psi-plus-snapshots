/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rtpbundlerouter.h"

namespace PsiMedia {
namespace {
    constexpr int MaxConfiguredSsrcs = 256;
}

quint16 RtpBundleRouter::read16(const QByteArray &data, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint16((quint16(p[0]) << 8) | quint16(p[1]));
}

quint32 RtpBundleRouter::read32(const QByteArray &data, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

void RtpBundleRouter::advanceRevision()
{
    ++revision_;
    if (!revision_)
        ++revision_;
}

bool RtpBundleRouter::configure(const QList<Route> &routes)
{
    if (routes.isEmpty() || routes.size() > MaxRoutes) {
        lastError_ = Error::InvalidRoutes;
        return false;
    }

    QHash<QByteArray, int>     endpointRoutes;
    QHash<QByteArray, int>     midRoutes;
    QHash<quint8, int>         payloadTypeRoutes;
    QSet<quint8>               ambiguousPayloadTypes;
    QHash<quint32, int>        incomingSsrcRoutes;
    QHash<quint32, int>        localSsrcRoutes;
    QHash<quint32, QByteArray> registeredOutgoingSsrcs;
    QSet<QByteArray>           mids;
    quint16                    midExtensionId = 0;

    auto addSsrc = [](QHash<quint32, int> &mapping, quint32 ssrc, int routeIndex) {
        if (!ssrc)
            return true;
        const auto it = mapping.constFind(ssrc);
        if (it != mapping.cend() && it.value() != routeIndex)
            return false;
        mapping.insert(ssrc, routeIndex);
        return true;
    };

    for (int routeIndex = 0; routeIndex < routes.size(); ++routeIndex) {
        const auto &route = routes.at(routeIndex);
        if (route.endpointId.isEmpty() || endpointRoutes.contains(route.endpointId)) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
        endpointRoutes.insert(route.endpointId, routeIndex);

        if (!route.mid.isEmpty()) {
            if (mids.contains(route.mid)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            mids.insert(route.mid);
        }
        if (route.midExtensionId) {
            if (route.midExtensionId > 255 || route.mid.isEmpty()
                || (midExtensionId && midExtensionId != route.midExtensionId)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            midExtensionId = route.midExtensionId;
            midRoutes.insert(route.mid, routeIndex);
        }

        for (auto payloadType : route.incomingPayloadTypes) {
            if (payloadType > 127) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            if (ambiguousPayloadTypes.contains(payloadType))
                continue;
            auto existing = payloadTypeRoutes.constFind(payloadType);
            if (existing == payloadTypeRoutes.cend()) {
                payloadTypeRoutes.insert(payloadType, routeIndex);
            } else if (existing.value() != routeIndex) {
                payloadTypeRoutes.remove(payloadType);
                ambiguousPayloadTypes.insert(payloadType);
            }
        }

        for (auto ssrc : route.incomingSsrcs) {
            if (!addSsrc(incomingSsrcRoutes, ssrc, routeIndex)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
        }
        for (auto ssrc : route.localSsrcs) {
            if (!addSsrc(localSsrcRoutes, ssrc, routeIndex)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
        }
        if (incomingSsrcRoutes.size() + localSsrcRoutes.size() > MaxConfiguredSsrcs) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
    }

    // Runtime registrations describe the actual producer rather than signaling
    // metadata. Preserve those belonging to surviving endpoints transactionally.
    for (auto it = registeredOutgoingSsrcs_.cbegin(); it != registeredOutgoingSsrcs_.cend(); ++it) {
        const auto routeIt = endpointRoutes.constFind(it.value());
        if (routeIt == endpointRoutes.cend())
            continue;
        if (!addSsrc(localSsrcRoutes, it.key(), routeIt.value())) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
        registeredOutgoingSsrcs.insert(it.key(), it.value());
    }

    routes_                  = routes;
    endpointRoutes_          = std::move(endpointRoutes);
    midRoutes_               = std::move(midRoutes);
    payloadTypeRoutes_       = std::move(payloadTypeRoutes);
    incomingSsrcRoutes_      = std::move(incomingSsrcRoutes);
    localSsrcRoutes_         = std::move(localSsrcRoutes);
    registeredOutgoingSsrcs_ = std::move(registeredOutgoingSsrcs);
    learnedSsrcs_.clear();
    midExtensionId_ = midExtensionId;
    advanceRevision();
    lastError_ = Error::None;
    return true;
}

void RtpBundleRouter::reset()
{
    routes_.clear();
    endpointRoutes_.clear();
    midRoutes_.clear();
    payloadTypeRoutes_.clear();
    incomingSsrcRoutes_.clear();
    localSsrcRoutes_.clear();
    learnedSsrcs_.clear();
    registeredOutgoingSsrcs_.clear();
    midExtensionId_ = 0;
    advanceRevision();
    lastError_ = Error::None;
}

bool RtpBundleRouter::registerOutgoingSsrc(const QByteArray &endpointId, quint32 ssrc)
{
    if (endpointId.isEmpty() || !ssrc) {
        lastError_ = Error::InvalidRoutes;
        return false;
    }
    const auto route = endpointRoutes_.constFind(endpointId);
    if (route == endpointRoutes_.cend()) {
        lastError_ = Error::UnknownRoute;
        return false;
    }

    const auto registered = registeredOutgoingSsrcs_.constFind(ssrc);
    if (registered != registeredOutgoingSsrcs_.cend()) {
        if (registered.value() != endpointId) {
            lastError_ = Error::AmbiguousRoute;
            return false;
        }
        lastError_ = Error::None;
        return true;
    }

    const auto existing = localSsrcRoutes_.constFind(ssrc);
    if (existing != localSsrcRoutes_.cend() && existing.value() != route.value()) {
        lastError_ = Error::AmbiguousRoute;
        return false;
    }
    if (registeredOutgoingSsrcs_.size() >= MaxRegisteredOutgoingSsrcs) {
        lastError_ = Error::ResourceLimit;
        return false;
    }

    if (existing == localSsrcRoutes_.cend())
        localSsrcRoutes_.insert(ssrc, route.value());
    registeredOutgoingSsrcs_.insert(ssrc, endpointId);
    lastError_ = Error::None;
    return true;
}

bool RtpBundleRouter::unregisterOutgoingSsrc(const QByteArray &endpointId, quint32 ssrc)
{
    const auto registered = registeredOutgoingSsrcs_.constFind(ssrc);
    if (registered == registeredOutgoingSsrcs_.cend()) {
        lastError_ = Error::None;
        return true;
    }
    if (registered.value() != endpointId) {
        lastError_ = Error::AmbiguousRoute;
        return false;
    }

    registeredOutgoingSsrcs_.remove(ssrc);
    const auto route = endpointRoutes_.constFind(endpointId);
    if (route != endpointRoutes_.cend() && !routes_.at(route.value()).localSsrcs.contains(ssrc)) {
        const auto local = localSsrcRoutes_.constFind(ssrc);
        if (local != localSsrcRoutes_.cend() && local.value() == route.value())
            localSsrcRoutes_.remove(ssrc);
    }
    lastError_ = Error::None;
    return true;
}

std::optional<RtpBundleRouter::ParsedRtp> RtpBundleRouter::parseRtp(const QByteArray &packet) const
{
    if (packet.size() < 12)
        return {};
    const auto *bytes = reinterpret_cast<const uchar *>(packet.constData());
    if ((bytes[0] >> 6) != 2)
        return {};

    const int csrcBytes = int(bytes[0] & 0x0f) * 4;
    if (csrcBytes > packet.size() - 12)
        return {};
    int offset = 12 + csrcBytes;

    ParsedRtp result;
    result.payloadType = bytes[1] & 0x7f;
    result.ssrc        = read32(packet, 8);

    if (bytes[0] & 0x10) {
        if (packet.size() - offset < 4)
            return {};

        const quint16 profile        = read16(packet, offset);
        const int     extensionBytes = int(read16(packet, offset + 2)) * 4;
        if (extensionBytes > packet.size() - offset - 4)
            return {};
        const int extensionEnd = offset + 4 + extensionBytes;

        if (midExtensionId_) {
            int cursor = offset + 4;
            if (profile == 0xbede && midExtensionId_ <= 14) {
                while (cursor < extensionEnd) {
                    const quint8 header = quint8(packet.at(cursor++));
                    if (!header)
                        continue;
                    const quint8 id = header >> 4;
                    if (id == 15)
                        break;
                    const int length = (header & 0x0f) + 1;
                    if (length > extensionEnd - cursor)
                        return {};
                    if (id == midExtensionId_) {
                        if (result.mid)
                            return {};
                        result.mid = packet.mid(cursor, length);
                        if (result.mid->isEmpty())
                            return {};
                    }
                    cursor += length;
                }
            } else if ((profile & 0xfff0) == 0x1000) {
                while (cursor < extensionEnd) {
                    const quint8 id = quint8(packet.at(cursor++));
                    if (!id)
                        continue;
                    if (cursor >= extensionEnd)
                        return {};
                    const int length = quint8(packet.at(cursor++));
                    if (length > extensionEnd - cursor)
                        return {};
                    if (id == midExtensionId_) {
                        if (result.mid || !length)
                            return {};
                        result.mid = packet.mid(cursor, length);
                    }
                    cursor += length;
                }
            }
        }
        offset = extensionEnd;
    }

    if (bytes[0] & 0x20) {
        if (offset >= packet.size())
            return {};
        const int paddingLength = quint8(packet.at(packet.size() - 1));
        if (!paddingLength || paddingLength > packet.size() - offset)
            return {};
    }
    return result;
}

bool RtpBundleRouter::payloadAllowed(int routeIndex, quint8 payloadType) const
{
    return routeIndex >= 0 && routeIndex < routes_.size()
        && routes_.at(routeIndex).incomingPayloadTypes.contains(payloadType);
}

std::optional<RtpBundleRouter::RoutedRtp> RtpBundleRouter::routed(int routeIndex, const QByteArray &packet)
{
    if (routeIndex < 0 || routeIndex >= routes_.size()) {
        lastError_ = Error::UnknownRoute;
        return {};
    }
    RoutedRtp result;
    result.endpointId = routes_.at(routeIndex).endpointId;
    result.data       = packet;
    result.revision   = revision_;
    lastError_        = Error::None;
    return result;
}

std::optional<RtpBundleRouter::RoutedRtp> RtpBundleRouter::routeIncomingRtp(const QByteArray &packet)
{
    auto parsed = parseRtp(packet);
    if (!parsed) {
        lastError_ = Error::MalformedPacket;
        return {};
    }

    auto ssrcRoute = incomingSsrcRoutes_.constFind(parsed->ssrc);
    if (parsed->mid) {
        auto midRoute = midRoutes_.constFind(*parsed->mid);
        if (midRoute == midRoutes_.cend()) {
            lastError_ = Error::UnknownRoute;
            return {};
        }
        if (ssrcRoute != incomingSsrcRoutes_.cend() && ssrcRoute.value() != midRoute.value()) {
            lastError_ = Error::AmbiguousRoute;
            return {};
        }
        if (!payloadAllowed(midRoute.value(), parsed->payloadType)) {
            lastError_ = Error::DisallowedPayloadType;
            return {};
        }
        if (ssrcRoute == incomingSsrcRoutes_.cend() && parsed->ssrc && learnedSsrcs_.size() < MaxLearnedSsrcs) {
            incomingSsrcRoutes_.insert(parsed->ssrc, midRoute.value());
            learnedSsrcs_.insert(parsed->ssrc);
        }
        return routed(midRoute.value(), packet);
    }

    if (ssrcRoute != incomingSsrcRoutes_.cend()) {
        if (!payloadAllowed(ssrcRoute.value(), parsed->payloadType)) {
            lastError_ = Error::DisallowedPayloadType;
            return {};
        }
        return routed(ssrcRoute.value(), packet);
    }

    const auto payloadRoute = payloadTypeRoutes_.constFind(parsed->payloadType);
    if (payloadRoute != payloadTypeRoutes_.cend()) {
        if (parsed->ssrc && learnedSsrcs_.size() < MaxLearnedSsrcs) {
            incomingSsrcRoutes_.insert(parsed->ssrc, payloadRoute.value());
            learnedSsrcs_.insert(parsed->ssrc);
        }
        return routed(payloadRoute.value(), packet);
    }

    lastError_ = Error::UnknownRoute;
    return {};
}

bool RtpBundleRouter::validateRtcp(const QByteArray &packet) const
{
    int offset = 0;
    while (offset < packet.size()) {
        if (offset + 4 > packet.size()) {
            lastError_ = Error::MalformedPacket;
            return false;
        }
        const auto *bytes = reinterpret_cast<const uchar *>(packet.constData() + offset);
        if ((bytes[0] >> 6) != 2) {
            lastError_ = Error::MalformedPacket;
            return false;
        }

        const bool   padding     = bytes[0] & 0x20;
        const int    count       = bytes[0] & 0x1f;
        const quint8 packetType  = bytes[1];
        const int    blockLength = (int(read16(packet, offset + 2)) + 1) * 4;
        if (blockLength < 4 || offset + blockLength > packet.size()) {
            lastError_ = Error::MalformedPacket;
            return false;
        }

        int payloadEnd = offset + blockLength;
        if (padding) {
            if (payloadEnd != packet.size()) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            const int paddingLength = quint8(packet.at(payloadEnd - 1));
            if (!paddingLength || paddingLength > blockLength - 4) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            payloadEnd -= paddingLength;
        }

        switch (packetType) {
        case 200:
            if (payloadEnd - offset < 28 + count * 24) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        case 201:
            if (payloadEnd - offset < 8 + count * 24) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        case 202: {
            int cursor = offset + 4;
            for (int chunk = 0; chunk < count; ++chunk) {
                if (cursor + 4 > payloadEnd) {
                    lastError_ = Error::MalformedPacket;
                    return false;
                }
                cursor += 4;
                bool ended = false;
                while (cursor < payloadEnd) {
                    const quint8 type = quint8(packet.at(cursor++));
                    if (!type) {
                        ended = true;
                        break;
                    }
                    if (cursor >= payloadEnd) {
                        lastError_ = Error::MalformedPacket;
                        return false;
                    }
                    const int length = quint8(packet.at(cursor++));
                    if (length > payloadEnd - cursor) {
                        lastError_ = Error::MalformedPacket;
                        return false;
                    }
                    cursor += length;
                }
                if (!ended) {
                    lastError_ = Error::MalformedPacket;
                    return false;
                }
                while ((cursor - offset) & 3) {
                    if (cursor >= payloadEnd || packet.at(cursor++) != 0) {
                        lastError_ = Error::MalformedPacket;
                        return false;
                    }
                }
            }
            if (cursor != payloadEnd) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        }
        case 203:
            if (payloadEnd - offset < 4 + count * 4) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        case 204:
            if (payloadEnd - offset < 12) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        case 205:
        case 206:
            if (payloadEnd - offset < 12) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        case 207:
            if (payloadEnd - offset < 8) {
                lastError_ = Error::MalformedPacket;
                return false;
            }
            break;
        default:
            // Authenticated, structurally valid unknown RTCP blocks are passed to
            // the group rtpsession. A block with no endpoint route must not make
            // an otherwise valid compound packet fail.
            break;
        }

        offset += blockLength;
    }

    const bool valid = offset == packet.size() && offset != 0;
    lastError_       = valid ? Error::None : Error::MalformedPacket;
    return valid;
}

bool RtpBundleRouter::isCurrent(const RoutedRtp &packet) const
{
    return packet.revision == revision_ && endpointRoutes_.contains(packet.endpointId);
}

} // namespace PsiMedia
