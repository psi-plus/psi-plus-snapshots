#include "../../src/xmpp/xmpp-im/jingle-rtp-router_p.h"
#include "../../src/xmpp/xmpp-im/jingle-rtp-description.h"

#include <QCoreApplication>

using namespace XMPP::Jingle;
using namespace XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void write16(QByteArray &data, int offset, quint16 value)
{
    data[offset]     = char(value >> 8);
    data[offset + 1] = char(value);
}

static void write32(QByteArray &data, int offset, quint32 value)
{
    data[offset]     = char(value >> 24);
    data[offset + 1] = char(value >> 16);
    data[offset + 2] = char(value >> 8);
    data[offset + 3] = char(value);
}

static void append16(QByteArray &data, quint16 value)
{
    data.append(char(value >> 8));
    data.append(char(value));
}

static QByteArray rtp(quint32 ssrc, const QByteArray &mid = {}, quint8 extensionId = 1, bool twoByte = false,
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

static QByteArray paddedRtp(quint32 ssrc, int paddingLength)
{
    auto packet = rtp(ssrc);
    packet[0]   = char(quint8(packet[0]) | 0x20);
    if (paddingLength > 0) {
        packet.append(QByteArray(paddingLength, '\0'));
        packet[packet.size() - 1] = char(paddingLength);
    }
    return packet;
}

static QByteArray rtcp(quint8 type, quint8 count, QByteArray payload)
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

static QByteArray senderReport(quint32 sender)
{
    QByteArray payload(24, '\0');
    write32(payload, 0, sender);
    return rtcp(200, 0, payload);
}

static QByteArray receiverReport(quint32 sender, quint32 reported)
{
    QByteArray payload(28, '\0');
    write32(payload, 0, sender);
    write32(payload, 4, reported);
    return rtcp(201, 1, payload);
}

static QByteArray receiverReport(quint32 sender)
{
    QByteArray payload(4, '\0');
    write32(payload, 0, sender);
    return rtcp(201, 0, payload);
}

static QByteArray receiverReport(quint32 sender, quint32 firstReported, quint32 secondReported)
{
    QByteArray payload(52, '\0');
    write32(payload, 0, sender);
    write32(payload, 4, firstReported);
    write32(payload, 28, secondReported);
    return rtcp(201, 2, payload);
}

static QByteArray feedback(quint8 type, quint32 sender, quint32 media, bool withFci = false)
{
    QByteArray payload(withFci ? 12 : 8, '\0');
    write32(payload, 0, sender);
    write32(payload, 4, media);
    return rtcp(type, 1, payload);
}

static QByteArray sdes(quint32 ssrc)
{
    QByteArray payload(4, '\0');
    write32(payload, 0, ssrc);
    payload.append(char(1)); // CNAME
    payload.append(char(1));
    payload.append('x');
    payload.append('\0'); // end
    return rtcp(202, 1, payload);
}

static BundleRouter::Route route(const char *name, const char *mid, quint16 midId, quint32 incoming, quint32 local,
                                 quint8 payloadType)
{
    BundleRouter::Route result;
    result.content        = ContentKey { QString::fromLatin1(name), Origin::Initiator };
    result.mid            = QByteArray(mid);
    result.midExtensionId = midId;
    result.incomingPayloadTypes.insert(payloadType);
    result.incomingSsrcs.insert(incoming);
    result.localSsrcs.insert(local);
    return result;
}

static bool containsContent(const QList<ContentKey> &contents, const ContentKey &content)
{
    return contents.contains(content);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    constexpr quint32 AudioRemote = 0x11111111;
    constexpr quint32 VideoRemote = 0x22222222;
    constexpr quint32 AudioLocal  = 0xaaaaaaaa;
    constexpr quint32 VideoLocal  = 0xbbbbbbbb;
    constexpr quint8  AudioPt     = 111;
    constexpr quint8  VideoPt     = 96;

    const auto audio = route("audio", "audio", 1, AudioRemote, AudioLocal, AudioPt);
    const auto video = route("video", "video", 1, VideoRemote, VideoLocal, VideoPt);

    BundleRouter router;
    check(router.configure({ audio, video }), "valid BUNDLE routes rejected");
    const auto initialRevision = router.revision();
    check(initialRevision != 0, "route revision was not initialized");

    auto audioPacket = router.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp);
    check(audioPacket && audioPacket->content == audio.content && router.isCurrent(*audioPacket),
          "known audio SSRC routed incorrectly");

    const quint32 learnedVideo = 0x33333333;
    auto videoByMid = router.routeIncoming(rtp(learnedVideo, "video", 1, false, VideoPt), SrtpContext::Packet::Rtp);
    check(videoByMid && videoByMid->content == video.content && router.learnedSsrcCount() == 1,
          "authenticated MID did not route and learn a new SSRC");
    auto videoByLearnedSsrc = router.routeIncoming(rtp(learnedVideo, {}, 1, false, VideoPt), SrtpContext::Packet::Rtp);
    check(videoByLearnedSsrc && videoByLearnedSsrc->content == video.content,
          "learned SSRC did not route without repeated MID");

    check(!router.routeIncoming(rtp(AudioRemote, "video", 1, false, VideoPt), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::AmbiguousRoute,
          "MID/SSRC route conflict was not dropped");
    check(!router.routeIncoming(rtp(0x44444444, "missing"), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::UnknownRoute,
          "unknown explicit MID fell back to an unrelated route");
    check(!router.routeIncoming(rtp(0x44444444, {}, 1, false, 127), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::UnknownRoute,
          "unknown RTP SSRC/PT was guessed");
    check(!router.routeIncoming(rtp(AudioLocal, {}, 1, false, 127), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::UnknownRoute,
          "local SSRC was incorrectly accepted as an incoming RTP route");

    // MID/SSRC select a route before PT fallback, but the selected content must
    // still have negotiated the packet PT.
    check(!router.routeIncoming(rtp(AudioRemote, {}, 1, false, 127), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::DisallowedPayloadType,
          "known SSRC accepted a PT not negotiated by any content");
    check(!router.routeIncoming(rtp(0x45454545, "audio", 1, false, 127), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::DisallowedPayloadType,
          "known MID accepted a PT not negotiated by any content");
    auto videoByPt = router.routeIncoming(rtp(0x46464646, {}, 1, false, VideoPt), SrtpContext::Packet::Rtp);
    check(videoByPt && videoByPt->content == video.content, "unique PT fallback did not route an unknown SSRC");

    auto malformedRtp = rtp(0x55555555, "audio");
    malformedRtp.chop(1);
    check(!router.routeIncoming(malformedRtp, SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::MalformedPacket,
          "truncated RTP extension accepted");
    auto truncatedCsrc = rtp(AudioRemote);
    truncatedCsrc[0]   = char(quint8(truncatedCsrc[0]) | 0x01);
    check(!router.routeIncoming(truncatedCsrc, SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::MalformedPacket,
          "truncated RTP CSRC list accepted");
    check(!router.routeIncoming(paddedRtp(AudioRemote, 0), SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::MalformedPacket,
          "RTP padding flag without padding accepted");
    auto excessivePadding                         = paddedRtp(AudioRemote, 2);
    excessivePadding[excessivePadding.size() - 1] = char(3);
    check(!router.routeIncoming(excessivePadding, SrtpContext::Packet::Rtp)
              && router.lastError() == BundleRouter::Error::MalformedPacket,
          "RTP padding beyond packet payload accepted");
    auto validPadding = router.routeIncoming(paddedRtp(AudioRemote, 4), SrtpContext::Packet::Rtp);
    check(validPadding && validPadding->content == audio.content, "valid RTP padding was rejected");

    BundleRouter twoByteRouter;
    auto         twoByteAudio   = audio;
    twoByteAudio.midExtensionId = 16;
    check(twoByteRouter.configure({ twoByteAudio }), "two-byte MID mapping rejected");
    auto twoBytePacket = twoByteRouter.routeIncoming(rtp(0x66666666, "audio", 16, true), SrtpContext::Packet::Rtp);
    check(twoBytePacket && twoBytePacket->content == audio.content, "two-byte RTP MID was not routed");

    const auto beforeInvalidRevision = router.revision();
    auto       duplicateMid          = video;
    duplicateMid.mid                 = audio.mid;
    check(!router.configure({ audio, duplicateMid }) && router.lastError() == BundleRouter::Error::InvalidRoutes
              && router.revision() == beforeInvalidRevision,
          "duplicate MID partially replaced the route table");
    check(bool(router.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp)),
          "failed configuration destroyed the previous route table");

    auto duplicateSsrc = video;
    duplicateSsrc.incomingSsrcs.clear();
    duplicateSsrc.incomingSsrcs.insert(AudioRemote);
    check(!router.configure({ audio, duplicateSsrc }), "one incoming SSRC was assigned to two contents");
    auto duplicateLocalSsrc = video;
    duplicateLocalSsrc.localSsrcs.clear();
    duplicateLocalSsrc.localSsrcs.insert(AudioLocal);
    check(!router.configure({ audio, duplicateLocalSsrc }), "one local SSRC was assigned to two contents");
    auto differentMidId           = video;
    differentMidId.midExtensionId = 2;
    check(!router.configure({ audio, differentMidId }), "different BUNDLE MID extension ids accepted");
    auto appbitsMid           = audio;
    appbitsMid.midExtensionId = 256;
    check(!router.configure({ appbitsMid }), "RFC 8285 appbits value used as a MID element id");

    // The same numeric SSRC may exist in opposite directions. RTP uses only the
    // peer/incoming map, while RTCP sender and media-source fields use their
    // protocol-defined direction.
    auto directionalVideo = video;
    directionalVideo.incomingSsrcs.clear();
    directionalVideo.incomingSsrcs.insert(AudioLocal);
    BundleRouter directional;
    check(directional.configure({ audio, directionalVideo }), "cross-direction SSRC collision was rejected");
    auto collidingRtp = directional.routeIncoming(rtp(AudioLocal, {}, 1, false, VideoPt), SrtpContext::Packet::Rtp);
    check(collidingRtp && collidingRtp->content == video.content, "incoming RTP used the local RTCP SSRC namespace");
    auto senderDirected = directional.routeIncoming(feedback(206, AudioLocal, 0), SrtpContext::Packet::Rtcp);
    check(senderDirected && senderDirected->content == video.content,
          "RTCP sender SSRC used the local media-source namespace");
    auto mediaDirected = directional.routeIncoming(feedback(206, 0, AudioLocal), SrtpContext::Packet::Rtcp);
    check(mediaDirected && mediaDirected->content == audio.content,
          "RTCP media SSRC used the incoming sender namespace");

    // Route-level revisions fence queued delivery after membership changes.
    auto queued = router.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp);
    check(queued && router.isCurrent(*queued), "failed to create queued route regression");
    check(router.configure({ audio }), "valid route table replacement failed");
    check(!router.isCurrent(*queued) && router.revision() != initialRevision && router.learnedSsrcCount() == 0,
          "route replacement did not invalidate stale delivery or learned sources");

    // RTCP is routed from all known sender/media/report SSRCs. A packet touching
    // multiple contents is preserved as one shared-media-session ingress rather
    // than duplicated or split between endpoints.
    BundleRouter rtcpRouter;
    check(rtcpRouter.configure({ audio, video }), "RTCP route table rejected");
    auto audioSr = rtcpRouter.routeIncoming(senderReport(AudioRemote), SrtpContext::Packet::Rtcp);
    check(audioSr && audioSr->delivery == BundleRouter::Delivery::Content && audioSr->content == audio.content,
          "RTCP sender report routed incorrectly");
    auto audioRr = rtcpRouter.routeIncoming(receiverReport(0x77777777, AudioLocal), SrtpContext::Packet::Rtcp);
    check(audioRr && audioRr->content == audio.content, "RTCP receiver report target was not routed");
    auto videoPli = rtcpRouter.routeIncoming(feedback(206, 0x88888888, VideoLocal), SrtpContext::Packet::Rtcp);
    check(videoPli && videoPli->content == video.content, "RTCP PLI media SSRC was not routed");
    auto videoNack = rtcpRouter.routeIncoming(feedback(205, 0x88888888, VideoLocal, true), SrtpContext::Packet::Rtcp);
    check(videoNack && videoNack->content == video.content, "RTCP NACK media SSRC was not routed");
    auto audioSdes = rtcpRouter.routeIncoming(sdes(AudioRemote), SrtpContext::Packet::Rtcp);
    check(audioSdes && audioSdes->content == audio.content, "RTCP SDES chunk was not routed");

    const auto crossContent   = senderReport(AudioRemote) + senderReport(VideoRemote);
    auto       sharedCompound = rtcpRouter.routeIncoming(crossContent, SrtpContext::Packet::Rtcp);
    check(sharedCompound && sharedCompound->delivery == BundleRouter::Delivery::SharedRtcp
              && sharedCompound->data == crossContent && sharedCompound->relatedContents.size() == 2
              && containsContent(sharedCompound->relatedContents, audio.content)
              && containsContent(sharedCompound->relatedContents, video.content)
              && rtcpRouter.isCurrent(*sharedCompound),
          "cross-content compound RTCP was not preserved for shared ingress");
    auto sharedReports
        = rtcpRouter.routeIncoming(receiverReport(0x89898989, AudioLocal, VideoLocal), SrtpContext::Packet::Rtcp);
    check(sharedReports && sharedReports->delivery == BundleRouter::Delivery::SharedRtcp
              && sharedReports->relatedContents.size() == 2,
          "one RR containing reports for two contents was rejected or split");
    check(!rtcpRouter.routeIncoming(senderReport(0x99999999), SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::UnknownRoute,
          "unknown RTCP sender was guessed");

    BundleRouter singleRtcp;
    check(singleRtcp.configure({ audio }), "single-content RTCP route table rejected");
    auto emptyRr = singleRtcp.routeIncoming(receiverReport(0x99999999), SrtpContext::Packet::Rtcp);
    check(emptyRr && emptyRr->delivery == BundleRouter::Delivery::Content && emptyRr->content == audio.content,
          "dedicated association dropped RTCP with no demuxing SSRC");
    check(!rtcpRouter.routeIncoming(rtcp(208, 0, QByteArray(4, '\0')), SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::MalformedPacket,
          "unknown RTCP packet type was attached to another compound route");
    auto malformedRtcp = senderReport(AudioRemote);
    write16(malformedRtcp, 2, 100);
    check(!rtcpRouter.routeIncoming(malformedRtcp, SrtpContext::Packet::Rtcp)
              && rtcpRouter.lastError() == BundleRouter::Error::MalformedPacket,
          "invalid RTCP block length accepted");

    // psimedia does not expose local SSRCs ahead of packet production. Runtime
    // registration lets RR/feedback route before the first remote RTP packet.
    auto dynamicAudio = audio;
    auto dynamicVideo = video;
    dynamicAudio.localSsrcs.clear();
    dynamicVideo.localSsrcs.clear();
    dynamicAudio.incomingSsrcs.clear();
    dynamicVideo.incomingSsrcs.clear();
    BundleRouter dynamic;
    check(dynamic.configure({ dynamicAudio, dynamicVideo }), "dynamic SSRC route table rejected");
    check(dynamic.registerOutgoingSsrc(dynamicAudio.content, AudioLocal), "audio outgoing SSRC registration failed");
    check(dynamic.registerOutgoingSsrc(dynamicVideo.content, VideoLocal), "video outgoing SSRC registration failed");
    check(dynamic.registeredOutgoingSsrcCount() == 2, "outgoing SSRC registrations were not tracked");
    auto earlyRr = dynamic.routeIncoming(receiverReport(0x77770000, AudioLocal), SrtpContext::Packet::Rtcp);
    check(earlyRr && earlyRr->content == dynamicAudio.content,
          "RR before first remote RTP did not use the registered local SSRC");
    auto earlyPli = dynamic.routeIncoming(feedback(206, 0, VideoLocal), SrtpContext::Packet::Rtcp);
    check(earlyPli && earlyPli->content == dynamicVideo.content,
          "PLI before first remote RTP did not use the registered local SSRC");
    auto earlyNack = dynamic.routeIncoming(feedback(205, 0, VideoLocal, true), SrtpContext::Packet::Rtcp);
    check(earlyNack && earlyNack->content == dynamicVideo.content,
          "NACK before first remote RTP did not use the registered local SSRC");
    check(!dynamic.registerOutgoingSsrc(dynamicVideo.content, AudioLocal)
              && dynamic.lastError() == BundleRouter::Error::AmbiguousRoute,
          "one outgoing SSRC was registered for two contents");

    // Removing one member retains the live source registration of the survivor
    // and drops the removed member's source without changing the old queued packet.
    auto queuedShared
        = dynamic.routeIncoming(receiverReport(0x77770000, AudioLocal, VideoLocal), SrtpContext::Packet::Rtcp);
    check(queuedShared && queuedShared->delivery == BundleRouter::Delivery::SharedRtcp,
          "shared RTCP teardown regression setup failed");
    check(dynamic.configure({ dynamicAudio }), "surviving member reconfiguration failed");
    check(dynamic.registeredOutgoingSsrcCount() == 1 && !dynamic.isCurrent(*queuedShared),
          "member removal did not fence queued shared RTCP or release its outgoing SSRC");
    auto survivingRr = dynamic.routeIncoming(receiverReport(0x77770000, AudioLocal), SrtpContext::Packet::Rtcp);
    check(survivingRr && survivingRr->content == dynamicAudio.content,
          "surviving member lost its runtime outgoing SSRC registration");
    check(!dynamic.routeIncoming(receiverReport(0x77770000, VideoLocal), SrtpContext::Packet::Rtcp)
              && dynamic.lastError() == BundleRouter::Error::UnknownRoute,
          "removed member retained an RTCP route");

    auto learnedAudio = dynamic.routeIncoming(rtp(0x77770000, {}, 1, false, AudioPt), SrtpContext::Packet::Rtp);
    check(learnedAudio && learnedAudio->content == dynamicAudio.content,
          "survivor sender-learning setup failed");
    check(!dynamic.routeIncoming(receiverReport(0x77770000, VideoLocal), SrtpContext::Packet::Rtcp)
              && dynamic.lastError() == BundleRouter::Error::UnknownRoute,
          "known survivor sender overrode an unresolved removed target");

    // Registering a source that is also present in signaling must still create
    // runtime ownership. Otherwise removing the static declaration on a later
    // reconfigure silently loses RTCP routing for the live sender.
    BundleRouter staticThenRuntime;
    auto         staticAudio = audio;
    staticAudio.incomingSsrcs.clear();
    check(staticThenRuntime.configure({ staticAudio }), "static/runtime SSRC route rejected");
    check(staticThenRuntime.registerOutgoingSsrc(staticAudio.content, AudioLocal),
          "runtime registration of a statically declared SSRC failed");
    check(staticThenRuntime.registeredOutgoingSsrcCount() == 1,
          "statically declared SSRC was not tracked as a runtime registration");
    staticAudio.localSsrcs.clear();
    check(staticThenRuntime.configure({ staticAudio }), "removing static SSRC declaration failed");
    auto afterStaticRemoval
        = staticThenRuntime.routeIncoming(receiverReport(0x77770001, AudioLocal), SrtpContext::Packet::Rtcp);
    check(afterStaticRemoval && afterStaticRemoval->content == staticAudio.content,
          "runtime SSRC route disappeared with its static declaration");
    check(staticThenRuntime.unregisterOutgoingSsrc(staticAudio.content, AudioLocal),
          "runtime unregister after static removal failed");
    check(!staticThenRuntime.routeIncoming(receiverReport(0x77770001, AudioLocal), SrtpContext::Packet::Rtcp)
              && staticThenRuntime.lastError() == BundleRouter::Error::UnknownRoute,
          "runtime unregister retained a removed static SSRC route");

    BundleRouter limitedOutgoing;
    auto         noStaticLocal = audio;
    noStaticLocal.localSsrcs.clear();
    check(limitedOutgoing.configure({ noStaticLocal }), "outgoing SSRC limit route rejected");
    for (int i = 0; i < BundleRouter::MaxRegisteredOutgoingSsrcs; ++i) {
        check(limitedOutgoing.registerOutgoingSsrc(noStaticLocal.content, 0x50000000u + quint32(i)),
              "outgoing SSRC registration hit its bound too early");
    }
    check(!limitedOutgoing.registerOutgoingSsrc(noStaticLocal.content, 0x60000000u)
              && limitedOutgoing.lastError() == BundleRouter::Error::ResourceLimit,
          "outgoing SSRC registration bound was not enforced");
    check(limitedOutgoing.unregisterOutgoingSsrc(noStaticLocal.content, 0x50000000u),
          "outgoing SSRC unregister failed");
    check(limitedOutgoing.registerOutgoingSsrc(noStaticLocal.content, 0x60000000u),
          "outgoing SSRC slot was not released after unregister");

    // A source first seen statically still consumes a runtime registration slot
    // once the producer registers it.
    BundleRouter limitedStaticRuntime;
    auto         limitedStatic = audio;
    limitedStatic.incomingSsrcs.clear();
    check(limitedStaticRuntime.configure({ limitedStatic }), "static runtime limit route rejected");
    check(limitedStaticRuntime.registerOutgoingSsrc(limitedStatic.content, AudioLocal),
          "static source runtime registration failed");
    for (int i = 0; i < BundleRouter::MaxRegisteredOutgoingSsrcs - 1; ++i) {
        check(limitedStaticRuntime.registerOutgoingSsrc(limitedStatic.content, 0x61000000u + quint32(i)),
              "static/runtime registration hit its bound too early");
    }
    check(!limitedStaticRuntime.registerOutgoingSsrc(limitedStatic.content, 0x62000000u)
              && limitedStaticRuntime.lastError() == BundleRouter::Error::ResourceLimit,
          "static source runtime registration did not count toward the bound");

    // Receive-only is valid without any local source registration. Unique PT can
    // still establish the authenticated incoming SSRC association.
    BundleRouter receiveOnly;
    auto         receiveOnlyAudio = dynamicAudio;
    check(receiveOnly.configure({ receiveOnlyAudio }), "receive-only route rejected");
    auto receivedByPt = receiveOnly.routeIncoming(rtp(0x70707070), SrtpContext::Packet::Rtp);
    check(receivedByPt && receivedByPt->content == receiveOnlyAudio.content,
          "receive-only content could not use unique PT fallback");

    // Incoming SSRC learning is bounded. MID still routes after the cap, but an
    // unlearned SSRC cannot later bypass MID-based demultiplexing.
    BundleRouter bounded;
    check(bounded.configure({ audio }), "bounded learning route rejected");
    quint32 last = 0;
    for (int i = 0; i < BundleRouter::MaxLearnedSsrcs + 1; ++i) {
        last        = 0x10000000u + quint32(i);
        auto routed = bounded.routeIncoming(rtp(last, "audio"), SrtpContext::Packet::Rtp);
        check(routed && routed->content == audio.content, "MID routing failed while learning SSRCs");
    }
    check(bounded.learnedSsrcCount() == BundleRouter::MaxLearnedSsrcs, "learned SSRC bound was not enforced");
    check(!bounded.routeIncoming(rtp(last, {}, 1, false, 127), SrtpContext::Packet::Rtp)
              && bounded.lastError() == BundleRouter::Error::UnknownRoute,
          "SSRC beyond the learning bound became an implicit route");

    auto beforeReset = bounded.routeIncoming(rtp(AudioRemote), SrtpContext::Packet::Rtp);
    check(beforeReset && bounded.isCurrent(*beforeReset), "reset revision regression setup failed");
    bounded.reset();
    check(!bounded.isCurrent(*beforeReset)
              && !bounded.routeIncoming(rtp(AudioRemote, {}, 1, false, 127), SrtpContext::Packet::Rtp)
              && bounded.lastError() == BundleRouter::Error::UnknownRoute,
          "reset retained a stale RTP route");


    // Accepted descriptions become directional route metadata. Peer sources
    // are incoming, local sources are RTCP feedback targets, and the accepted
    // answer controls the usable PT/MID set.
    Description localDescription;
    localDescription.media   = QStringLiteral("audio");
    localDescription.rtcpMux = true;
    localDescription.ssrc    = AudioLocal;
    localDescription.sources.append(Source { AudioLocal + 1, {} });
    localDescription.payloads.append(PayloadType { AudioPt, QStringLiteral("opus"), 48000, 2 });
    localDescription.headerExtensions.append(
        HeaderExtension { 3, QStringLiteral("urn:ietf:params:rtp-hdrext:sdes:mid"), Origin::Both, {} });

    Description remoteDescription = localDescription;
    remoteDescription.ssrc        = AudioRemote;
    remoteDescription.sources     = { Source { AudioRemote + 1, {} } };
    remoteDescription.payloads.first().id = 109;

    const ContentKey describedContent { QStringLiteral("voice"), Origin::Initiator };
    auto describedRoute
        = bundleRouteForDescriptions(describedContent, true, localDescription, remoteDescription);
    check(describedRoute && describedRoute->content == describedContent
              && describedRoute->incomingPayloadTypes == QSet<quint8> { 109 }
              && describedRoute->incomingSsrcs.contains(AudioRemote)
              && describedRoute->incomingSsrcs.contains(AudioRemote + 1)
              && describedRoute->localSsrcs.contains(AudioLocal)
              && describedRoute->localSsrcs.contains(AudioLocal + 1)
              && describedRoute->mid == QByteArrayLiteral("voice") && describedRoute->midExtensionId == 3,
          "locally-created negotiated descriptions produced the wrong BUNDLE route");

    auto responderRoute
        = bundleRouteForDescriptions(ContentKey { QStringLiteral("voice"), Origin::Initiator }, false,
                                     remoteDescription, localDescription);
    check(responderRoute && responderRoute->incomingPayloadTypes == QSet<quint8> { 109 }
              && responderRoute->incomingSsrcs.contains(AudioLocal)
              && responderRoute->localSsrcs.contains(AudioRemote),
          "remotely-created negotiated descriptions reversed route direction");

    auto noMuxDescription = localDescription;
    noMuxDescription.rtcpMux = false;
    check(!bundleRouteForDescriptions(describedContent, true, noMuxDescription, remoteDescription),
          "non-muxed RTP was accepted as a BUNDLE route");

    qInfo("RTP BUNDLE router regressions passed");
}
