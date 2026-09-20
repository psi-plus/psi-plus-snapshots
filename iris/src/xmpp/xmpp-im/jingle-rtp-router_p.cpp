// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-router_p.h"
#include "jingle-rtp-description.h"

#include <algorithm>

namespace XMPP::Jingle::RTP {
namespace {
    constexpr int MaxConfiguredSsrcs = 256;
}

quint16 BundleRouter::read16(const QByteArray &data, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return quint16((quint16(p[0]) << 8) | quint16(p[1]));
}

quint32 BundleRouter::read32(const QByteArray &data, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

void BundleRouter::noteSsrc(const QHash<quint32, int> &mapping, quint32 ssrc, QSet<int> &routes)
{
    if (!ssrc)
        return; // zero has protocol-specific wildcard meanings in RTCP feedback.
    const auto it = mapping.constFind(ssrc);
    if (it != mapping.cend())
        routes.insert(it.value());
}

void BundleRouter::advanceRevision()
{
    ++revision_;
    if (!revision_)
        ++revision_;
}

namespace {
constexpr auto MidUri = "urn:ietf:params:rtp-hdrext:sdes:mid";

void appendSources(QSet<quint32> &target, const Description &description)
{
    if (description.ssrc && *description.ssrc)
        target.insert(*description.ssrc);
    for (const auto &source : description.sources) {
        if (source.ssrc)
            target.insert(source.ssrc);
    }
}
}

std::optional<BundleRouter::Route> bundleRouteForDescriptions(const ContentKey &content, bool localContent,
                                                               const Description &local,
                                                               const Description &remote)
{
    if (content.first.isEmpty() || local.media.isEmpty() || remote.media.isEmpty() || local.media != remote.media
        || !local.rtcpMux || !remote.rtcpMux)
        return std::nullopt;

    // Current negotiation preserves payload identifiers. The answer is the
    // accepted set in both directions: remote for locally-created content,
    // local for remotely-created content.
    const auto &accepted = localContent ? remote : local;
    if (accepted.payloads.isEmpty())
        return std::nullopt;

    BundleRouter::Route route;
    route.content = content;
    for (const auto &payload : accepted.payloads)
        route.incomingPayloadTypes.insert(payload.id);

    // Source declarations describe the endpoint that emitted that description.
    appendSources(route.incomingSsrcs, remote);
    appendSources(route.localSsrcs, local);

    // MID is useful only when it survived offer/answer negotiation. The content
    // name is the Jingle grouping identity corresponding to SDP MID semantics.
    for (const auto &extension : accepted.headerExtensions) {
        if (extension.uri == QLatin1String(MidUri)) {
            route.midExtensionId = extension.id;
            route.mid            = content.first.toUtf8();
            break;
        }
    }
    return route;
}

bool BundleRouter::configure(const QList<Route> &routes)
{
    if (routes.isEmpty() || routes.size() > MaxRoutes) {
        lastError_ = Error::InvalidRoutes;
        return false;
    }

    QMap<ContentKey, int>      contentRoutes;
    QHash<QByteArray, int>     midRoutes;
    QHash<quint8, int>         payloadTypeRoutes;
    QSet<quint8>               ambiguousPayloadTypes;
    QHash<quint32, int>        incomingSsrcRoutes;
    QHash<quint32, int>        localSsrcRoutes;
    QHash<quint32, ContentKey> registeredOutgoingSsrcs;
    QSet<QByteArray>           mids;
    quint16                    midExtensionId = 0;

    auto addSsrc = [](QHash<quint32, int> &mapping, quint32 ssrc, int routeIndex) {
        auto it = mapping.constFind(ssrc);
        if (it != mapping.cend() && it.value() != routeIndex)
            return false;
        mapping.insert(ssrc, routeIndex);
        return true;
    };

    for (int routeIndex = 0; routeIndex < routes.size(); ++routeIndex) {
        const auto &route = routes.at(routeIndex);
        if (route.content.first.isEmpty()
            || (route.content.second != Origin::Initiator && route.content.second != Origin::Responder)
            || contentRoutes.contains(route.content)) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
        contentRoutes.insert(route.content, routeIndex);

        if (!route.mid.isEmpty()) {
            if (mids.contains(route.mid)) {
                lastError_ = Error::InvalidRoutes;
                return false;
            }
            mids.insert(route.mid);
        }
        if (route.midExtensionId) {
            // 256 is the RFC 8285 appbits signaling value, not an RTP extension
            // element id. Extended offer-only ids are likewise unusable on wire.
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

    // Runtime registrations describe actual local sources rather than signaling
    // metadata. Keep those belonging to surviving contents across membership-only
    // reconfiguration, but validate them transactionally against the new table.
    for (auto it = registeredOutgoingSsrcs_.cbegin(); it != registeredOutgoingSsrcs_.cend(); ++it) {
        const auto routeIt = contentRoutes.constFind(it.value());
        if (routeIt == contentRoutes.cend())
            continue;
        if (!addSsrc(localSsrcRoutes, it.key(), routeIt.value())) {
            lastError_ = Error::InvalidRoutes;
            return false;
        }
        registeredOutgoingSsrcs.insert(it.key(), it.value());
    }

    routes_                  = routes;
    contentRoutes_           = std::move(contentRoutes);
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

void BundleRouter::reset()
{
    routes_.clear();
    contentRoutes_.clear();
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

bool BundleRouter::registerOutgoingSsrc(const ContentKey &content, quint32 ssrc)
{
    if (!ssrc) {
        lastError_ = Error::InvalidRoutes;
        return false;
    }
    const auto route = contentRoutes_.constFind(content);
    if (route == contentRoutes_.cend()) {
        lastError_ = Error::UnknownRoute;
        return false;
    }

    const auto registered = registeredOutgoingSsrcs_.constFind(ssrc);
    if (registered != registeredOutgoingSsrcs_.cend()) {
        if (registered.value() != content) {
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

    // Runtime registration records the actual producer identity independently
    // from any static source declaration. This lets it survive a later
    // reconfiguration that removes the statically signalled SSRC.
    if (existing == localSsrcRoutes_.cend())
        localSsrcRoutes_.insert(ssrc, route.value());
    registeredOutgoingSsrcs_.insert(ssrc, content);
    lastError_ = Error::None;
    return true;
}

bool BundleRouter::unregisterOutgoingSsrc(const ContentKey &content, quint32 ssrc)
{
    const auto registered = registeredOutgoingSsrcs_.constFind(ssrc);
    if (registered == registeredOutgoingSsrcs_.cend()) {
        lastError_ = Error::None;
        return true;
    }
    if (registered.value() != content) {
        lastError_ = Error::AmbiguousRoute;
        return false;
    }

    registeredOutgoingSsrcs_.remove(ssrc);
    const auto route = contentRoutes_.constFind(content);
    if (route != contentRoutes_.cend() && !routes_.at(route.value()).localSsrcs.contains(ssrc)) {
        const auto local = localSsrcRoutes_.constFind(ssrc);
        if (local != localSsrcRoutes_.cend() && local.value() == route.value())
            localSsrcRoutes_.remove(ssrc);
    }
    lastError_ = Error::None;
    return true;
}

std::optional<BundleRouter::ParsedRtp> BundleRouter::parseRtp(const QByteArray &packet) const
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
                        break; // RFC 8285 reserved value terminates extension processing.
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

bool BundleRouter::collectRtcpRoutes(const QByteArray &packet, QSet<int> &routes, bool &unresolvedTarget) const
{
    unresolvedTarget = false;
    auto noteLocalTarget = [this, &routes, &unresolvedTarget](quint32 ssrc) {
        if (!ssrc)
            return;
        const auto route = localSsrcRoutes_.constFind(ssrc);
        if (route == localSsrcRoutes_.cend()) {
            unresolvedTarget = true;
            return;
        }
        routes.insert(route.value());
    };

    int offset = 0;
    while (offset < packet.size()) {
        if (offset + 4 > packet.size())
            return false;
        const auto *bytes = reinterpret_cast<const uchar *>(packet.constData() + offset);
        if ((bytes[0] >> 6) != 2)
            return false;

        const bool   padding     = bytes[0] & 0x20;
        const int    count       = bytes[0] & 0x1f;
        const quint8 packetType  = bytes[1];
        const int    blockLength = (int(read16(packet, offset + 2)) + 1) * 4;
        if (blockLength < 4 || offset + blockLength > packet.size())
            return false;

        int payloadEnd = offset + blockLength;
        if (padding) {
            if (payloadEnd != packet.size())
                return false; // Only the last packet in a compound RTCP packet may be padded.
            const int paddingLength = quint8(packet.at(payloadEnd - 1));
            if (!paddingLength || paddingLength > blockLength - 4)
                return false;
            payloadEnd -= paddingLength;
        }

        switch (packetType) {
        case 200: { // Sender Report
            if (payloadEnd - offset < 28 + count * 24)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            for (int i = 0; i < count; ++i)
                noteLocalTarget(read32(packet, offset + 28 + i * 24));
            break;
        }
        case 201: { // Receiver Report
            if (payloadEnd - offset < 8 + count * 24)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            for (int i = 0; i < count; ++i)
                noteLocalTarget(read32(packet, offset + 8 + i * 24));
            break;
        }
        case 202: { // SDES
            int cursor = offset + 4;
            for (int chunk = 0; chunk < count; ++chunk) {
                if (cursor + 4 > payloadEnd)
                    return false;
                noteSsrc(incomingSsrcRoutes_, read32(packet, cursor), routes);
                cursor += 4;
                bool ended = false;
                while (cursor < payloadEnd) {
                    const quint8 type = quint8(packet.at(cursor++));
                    if (!type) {
                        ended = true;
                        break;
                    }
                    if (cursor >= payloadEnd)
                        return false;
                    const int length = quint8(packet.at(cursor++));
                    if (length > payloadEnd - cursor)
                        return false;
                    cursor += length;
                }
                if (!ended)
                    return false;
                while ((cursor - offset) & 3) {
                    if (cursor >= payloadEnd || packet.at(cursor++) != 0)
                        return false;
                }
            }
            if (cursor != payloadEnd)
                return false;
            break;
        }
        case 203: // BYE
            if (payloadEnd - offset < 4 + count * 4)
                return false;
            for (int i = 0; i < count; ++i)
                noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4 + i * 4), routes);
            break;
        case 204: // APP
            if (payloadEnd - offset < 12)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            break;
        case 205: // RTPFB
        case 206: // PSFB
            if (payloadEnd - offset < 12)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            noteLocalTarget(read32(packet, offset + 8));
            break;
        case 207: // XR
            if (payloadEnd - offset < 8)
                return false;
            noteSsrc(incomingSsrcRoutes_, read32(packet, offset + 4), routes);
            break;
        default:
            // Unknown RTCP packet types may contain routing SSRCs we do not know
            // how to interpret. Fail closed instead of attaching them to a content
            // selected by a different block in the same authenticated compound.
            return false;
        }
        offset += blockLength;
    }
    return offset == packet.size();
}

bool BundleRouter::payloadAllowed(int routeIndex, quint8 payloadType) const
{
    return routeIndex >= 0 && routeIndex < routes_.size()
        && routes_.at(routeIndex).incomingPayloadTypes.contains(payloadType);
}

std::optional<BundleRouter::RoutedPacket> BundleRouter::routed(int routeIndex, const QByteArray &packet,
                                                               SrtpContext::Packet kind)
{
    if (routeIndex < 0 || routeIndex >= routes_.size()) {
        lastError_ = Error::UnknownRoute;
        return {};
    }
    RoutedPacket result;
    result.delivery = Delivery::Content;
    result.content  = routes_.at(routeIndex).content;
    result.relatedContents.append(result.content);
    result.data     = packet;
    result.kind     = kind;
    result.revision = revision_;
    lastError_      = Error::None;
    return result;
}

std::optional<BundleRouter::RoutedPacket> BundleRouter::routedSharedRtcp(const QSet<int>  &routeIndexes,
                                                                         const QByteArray &packet)
{
    QList<int> ordered = routeIndexes.values();
    std::sort(ordered.begin(), ordered.end());
    if (ordered.size() < 2) {
        lastError_ = Error::UnknownRoute;
        return {};
    }

    RoutedPacket result;
    result.delivery = Delivery::SharedRtcp;
    result.data     = packet;
    result.kind     = SrtpContext::Packet::Rtcp;
    result.revision = revision_;
    for (int routeIndex : ordered) {
        if (routeIndex < 0 || routeIndex >= routes_.size()) {
            lastError_ = Error::UnknownRoute;
            return {};
        }
        result.relatedContents.append(routes_.at(routeIndex).content);
    }
    lastError_ = Error::None;
    return result;
}

std::optional<BundleRouter::RoutedPacket> BundleRouter::routeIncoming(const QByteArray   &packet,
                                                                      SrtpContext::Packet kind)
{
    if (kind == SrtpContext::Packet::Rtp) {
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
            return routed(midRoute.value(), packet, kind);
        }

        if (ssrcRoute != incomingSsrcRoutes_.cend()) {
            if (!payloadAllowed(ssrcRoute.value(), parsed->payloadType)) {
                lastError_ = Error::DisallowedPayloadType;
                return {};
            }
            return routed(ssrcRoute.value(), packet, kind);
        }

        const auto payloadRoute = payloadTypeRoutes_.constFind(parsed->payloadType);
        if (payloadRoute != payloadTypeRoutes_.cend()) {
            if (parsed->ssrc && learnedSsrcs_.size() < MaxLearnedSsrcs) {
                incomingSsrcRoutes_.insert(parsed->ssrc, payloadRoute.value());
                learnedSsrcs_.insert(parsed->ssrc);
            }
            return routed(payloadRoute.value(), packet, kind);
        }

        lastError_ = Error::UnknownRoute;
        return {};
    }

    QSet<int> routes;
    bool      unresolvedTarget = false;
    if (!collectRtcpRoutes(packet, routes, unresolvedTarget)) {
        lastError_ = Error::MalformedPacket;
        return {};
    }
    // Report/media SSRCs refer to our own producers. If one is explicitly
    // present but no longer belongs to this association, never reassign that
    // packet to a surviving member after BUNDLE membership changes.
    if (unresolvedTarget) {
        lastError_ = Error::UnknownRoute;
        return {};
    }
    if (routes.isEmpty()) {
        // On a dedicated SRTP association there is no demultiplexing ambiguity
        // for sender-only RTCP such as an empty Receiver Report. Shared BUNDLE
        // associations must still fail closed unless the packet identifies a
        // content.
        if (routes_.size() == 1)
            return routed(0, packet, kind);
        lastError_ = Error::UnknownRoute;
        return {};
    }
    if (routes.size() == 1)
        return routed(*routes.cbegin(), packet, kind);
    return routedSharedRtcp(routes, packet);
}

bool BundleRouter::isCurrent(const RoutedPacket &packet) const
{
    if (packet.revision != revision_)
        return false;
    if (packet.delivery == Delivery::Content)
        return contentRoutes_.contains(packet.content);
    if (packet.delivery != Delivery::SharedRtcp || packet.relatedContents.size() < 2)
        return false;
    for (const auto &content : packet.relatedContents) {
        if (!contentRoutes_.contains(content))
            return false;
    }
    return true;
}

}
