/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "rtpbundlerouter.h"

#include <QCoreApplication>

using namespace PsiMedia;

namespace {

void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

void write16(QByteArray &data, int offset, quint16 value)
{
    data[offset]     = char(value >> 8);
    data[offset + 1] = char(value);
}

void write32(QByteArray &data, int offset, quint32 value)
{
    data[offset]     = char(value >> 24);
    data[offset + 1] = char(value >> 16);
    data[offset + 2] = char(value >> 8);
    data[offset + 3] = char(value);
}

void append16(QByteArray &data, quint16 value)
{
    data.append(char(value >> 8));
    data.append(char(value));
}

QByteArray rtp(quint32 ssrc, const QByteArray &mid = {}, quint8 extensionId = 1, bool twoByte = false,
               quint8 payloadType = 111)
{
    QByteArray packet(12, '\0');
    packet[0] = char(0x80 | (mid.isEmpty() ? 0 : 0x10));
    packet[1] = char(payloadType & 0x7f);
    write32(packet, 8, ssrc);
    if (mid.isEmpty())
        return packet;

    QByteArray extensions;
    if (twoByte) {
        extensions.append(char(extensionId));
        extensions.append(char(mid.size()));
        extensions.append(mid);
    } else {
        check(extensionId <= 14 && !mid.isEmpty() && mid.size() <= 16, "invalid one-byte test extension");
        extensions.append(char((extensionId << 4) | (mid.size() - 1)));
        extensions.append(mid);
    }
    while (extensions.size() & 3)
        extensions.append('\0');
    append16(packet, twoByte ? 0x1000 : 0xbede);
    append16(packet, quint16(extensions.size() / 4));
    packet.append(extensions);
    return packet;
}

QByteArray paddedRtp(quint32 ssrc, int paddingLength)
{
    auto packet = rtp(ssrc);
    packet[0]   = char(quint8(packet[0]) | 0x20);
    if (paddingLength > 0) {
        packet.append(QByteArray(paddingLength, '\0'));
        packet[packet.size() - 1] = char(paddingLength);
    }
    return packet;
}

QByteArray rtcp(quint8 type, quint8 count, QByteArray payload)
{
    while ((payload.size() + 4) & 3)
        payload.append('\0');
    QByteArray packet(4, '\0');
    packet[0] = char(0x80 | (count & 0x1f));
    packet[1] = char(type);
    packet.append(payload);
    write16(packet, 2, quint16(packet.size() / 4 - 1));
    return packet;
}

QByteArray senderReport(quint32 sender)
{
    QByteArray payload(24, '\0');
    write32(payload, 0, sender);
    return rtcp(200, 0, payload);
}

QByteArray receiverReport(quint32 sender, quint32 reported)
{
    QByteArray payload(28, '\0');
    write32(payload, 0, sender);
    write32(payload, 4, reported);
    return rtcp(201, 1, payload);
}

RtpBundleRouter::Route route(const char *id, const char *mid, quint16 midId, quint32 incoming, quint32 local,
                             quint8 payloadType)
{
    RtpBundleRouter::Route result;
    result.endpointId     = QByteArray(id);
    result.mid            = QByteArray(mid);
    result.midExtensionId = midId;
    result.incomingPayloadTypes.insert(payloadType);
    result.incomingSsrcs.insert(incoming);
    result.localSsrcs.insert(local);
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    constexpr quint32 AudioRemote = 0x11111111;
    constexpr quint32 VideoRemote = 0x22222222;
    constexpr quint32 AudioLocal  = 0xaaaaaaaa;
    constexpr quint32 VideoLocal  = 0xbbbbbbbb;
    constexpr quint8  AudioPt     = 111;
    constexpr quint8  VideoPt     = 96;

    const auto audio = route("audio-endpoint", "audio", 1, AudioRemote, AudioLocal, AudioPt);
    const auto video = route("video-endpoint", "video", 1, VideoRemote, VideoLocal, VideoPt);

    RtpBundleRouter router;
    check(router.configure({ audio, video }), "valid BUNDLE routes rejected");
    const auto initialRevision = router.revision();

    auto audioPacket = router.routeIncomingRtp(rtp(AudioRemote));
    check(audioPacket && audioPacket->endpointId == audio.endpointId && router.isCurrent(*audioPacket),
          "known audio SSRC routed incorrectly");

    const quint32 learnedVideo = 0x33333333;
    auto          videoByMid   = router.routeIncomingRtp(rtp(learnedVideo, "video", 1, false, VideoPt));
    check(videoByMid && videoByMid->endpointId == video.endpointId && router.learnedSsrcCount() == 1,
          "authenticated MID did not route and learn a new SSRC");
    auto videoByLearned = router.routeIncomingRtp(rtp(learnedVideo, {}, 1, false, VideoPt));
    check(videoByLearned && videoByLearned->endpointId == video.endpointId,
          "learned SSRC did not route without repeated MID");

    check(!router.routeIncomingRtp(rtp(AudioRemote, "video", 1, false, VideoPt))
              && router.lastError() == RtpBundleRouter::Error::AmbiguousRoute,
          "MID/SSRC route conflict was not dropped");
    check(!router.routeIncomingRtp(rtp(0x44444444, "missing"))
              && router.lastError() == RtpBundleRouter::Error::UnknownRoute,
          "unknown explicit MID fell back to another route");
    check(!router.routeIncomingRtp(rtp(AudioRemote, {}, 1, false, 127))
              && router.lastError() == RtpBundleRouter::Error::DisallowedPayloadType,
          "known SSRC accepted an unnegotiated PT");

    auto videoByPt = router.routeIncomingRtp(rtp(0x46464646, {}, 1, false, VideoPt));
    check(videoByPt && videoByPt->endpointId == video.endpointId, "unique PT fallback failed");

    auto malformed = rtp(0x55555555, "audio");
    malformed.chop(1);
    check(!router.routeIncomingRtp(malformed) && router.lastError() == RtpBundleRouter::Error::MalformedPacket,
          "truncated RTP extension accepted");

    auto truncatedCsrc = rtp(AudioRemote);
    truncatedCsrc[0]   = char(quint8(truncatedCsrc[0]) | 0x01);
    check(!router.routeIncomingRtp(truncatedCsrc) && router.lastError() == RtpBundleRouter::Error::MalformedPacket,
          "truncated CSRC list accepted");

    check(!router.routeIncomingRtp(paddedRtp(AudioRemote, 0))
              && router.lastError() == RtpBundleRouter::Error::MalformedPacket,
          "padding flag without padding accepted");
    auto validPadding = router.routeIncomingRtp(paddedRtp(AudioRemote, 4));
    check(validPadding && validPadding->endpointId == audio.endpointId, "valid RTP padding rejected");

    RtpBundleRouter twoByte;
    auto            twoByteAudio = audio;
    twoByteAudio.midExtensionId  = 16;
    check(twoByte.configure({ twoByteAudio }), "two-byte MID mapping rejected");
    auto twoBytePacket = twoByte.routeIncomingRtp(rtp(0x66666666, "audio", 16, true));
    check(twoBytePacket && twoBytePacket->endpointId == audio.endpointId, "two-byte MID routing failed");

    const auto beforeInvalid = router.revision();
    auto       duplicateMid  = video;
    duplicateMid.mid         = audio.mid;
    check(!router.configure({ audio, duplicateMid }) && router.lastError() == RtpBundleRouter::Error::InvalidRoutes
              && router.revision() == beforeInvalid,
          "invalid route table partially committed");
    check(bool(router.routeIncomingRtp(rtp(AudioRemote))), "failed configure destroyed old table");

    auto differentMidId           = video;
    differentMidId.midExtensionId = 2;
    check(!router.configure({ audio, differentMidId }), "different BUNDLE MID extension ids accepted");

    auto appbitsMid           = audio;
    appbitsMid.midExtensionId = 256;
    check(!router.configure({ appbitsMid }), "RFC 8285 appbits value used as MID element id");

    auto queued = router.routeIncomingRtp(rtp(AudioRemote));
    check(queued && router.isCurrent(*queued), "queued route setup failed");
    check(router.configure({ audio }), "valid route replacement failed");
    check(!router.isCurrent(*queued) && router.revision() != initialRevision && router.learnedSsrcCount() == 0,
          "route replacement did not fence stale delivery");

    // Runtime local SSRC registration survives route refresh for a surviving
    // endpoint and is released with a removed endpoint.
    auto dynamicAudio = audio;
    auto dynamicVideo = video;
    dynamicAudio.localSsrcs.clear();
    dynamicVideo.localSsrcs.clear();
    RtpBundleRouter dynamic;
    check(dynamic.configure({ dynamicAudio, dynamicVideo }), "dynamic route table rejected");
    check(dynamic.registerOutgoingSsrc(dynamicAudio.endpointId, AudioLocal), "audio SSRC registration failed");
    check(dynamic.registerOutgoingSsrc(dynamicVideo.endpointId, VideoLocal), "video SSRC registration failed");
    check(dynamic.registeredOutgoingSsrcCount() == 2, "outgoing SSRC registrations missing");
    check(!dynamic.registerOutgoingSsrc(dynamicVideo.endpointId, AudioLocal)
              && dynamic.lastError() == RtpBundleRouter::Error::AmbiguousRoute,
          "one outgoing SSRC registered for two endpoints");
    check(dynamic.configure({ dynamicAudio }), "surviving endpoint reconfiguration failed");
    check(dynamic.registeredOutgoingSsrcCount() == 1, "removed endpoint retained outgoing SSRC state");

    RtpBundleRouter limited;
    auto            noStatic = audio;
    noStatic.localSsrcs.clear();
    check(limited.configure({ noStatic }), "outgoing SSRC limit route rejected");
    for (int i = 0; i < RtpBundleRouter::MaxRegisteredOutgoingSsrcs; ++i)
        check(limited.registerOutgoingSsrc(noStatic.endpointId, 0x50000000u + quint32(i)),
              "outgoing SSRC limit hit too early");
    check(!limited.registerOutgoingSsrc(noStatic.endpointId, 0x60000000u)
              && limited.lastError() == RtpBundleRouter::Error::ResourceLimit,
          "outgoing SSRC bound not enforced");

    // RTCP belongs to the whole group. Known and unknown block types are accepted
    // when structurally valid; no endpoint match is required.
    const auto compound = senderReport(AudioRemote) + senderReport(VideoRemote);
    check(router.validateRtcp(compound), "cross-media compound RTCP rejected");
    check(router.validateRtcp(receiverReport(0x77777777, AudioLocal)), "receiver report rejected");
    check(router.validateRtcp(rtcp(208, 0, QByteArray(4, '\0'))), "structurally valid unknown RTCP block rejected");

    auto malformedRtcp = senderReport(AudioRemote);
    write16(malformedRtcp, 2, 100);
    check(!router.validateRtcp(malformedRtcp) && router.lastError() == RtpBundleRouter::Error::MalformedPacket,
          "invalid RTCP block length accepted");

    auto unknownCompound = senderReport(AudioRemote) + rtcp(208, 0, QByteArray(4, '\0')) + senderReport(VideoRemote);
    check(router.validateRtcp(unknownCompound), "unknown RTCP block poisoned valid compound packet");

    qInfo("psimedia RTP BUNDLE router regressions passed");
    return 0;
}
