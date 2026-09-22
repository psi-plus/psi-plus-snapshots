/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "securertpgroup.h"
#include "gstrtpsessioncontext.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QtEndian>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

using namespace PsiMedia;
using namespace std::chrono_literals;

namespace {

void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

template <typename Predicate> bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void put16(uchar *p, quint16 value)
{
    p[0] = uchar(value >> 8);
    p[1] = uchar(value);
}

void put32(uchar *p, quint32 value)
{
    p[0] = uchar(value >> 24);
    p[1] = uchar(value >> 16);
    p[2] = uchar(value >> 8);
    p[3] = uchar(value);
}

QByteArray makeRtp(quint8 pt, quint16 sequence, quint32 timestamp, quint32 ssrc)
{
    QByteArray packet(13, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x80;
    p[1]         = pt;
    put16(p + 2, sequence);
    put32(p + 4, timestamp);
    put32(p + 8, ssrc);
    p[12] = 0x7f;
    return packet;
}

GstBuffer *bufferFor(const QByteArray &data, GstClockTime pts)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, gsize(data.size()), nullptr);
    check(buffer != nullptr, "failed to allocate RTP buffer");
    check(gst_buffer_fill(buffer, 0, data.constData(), gsize(data.size())) == gsize(data.size()),
          "failed to fill RTP buffer");
    GST_BUFFER_PTS(buffer)      = pts;
    GST_BUFFER_DTS(buffer)      = pts;
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    return buffer;
}

PPayloadInfo payload(int id, const char *name, int clockrate, int channels = -1)
{
    PPayloadInfo result;
    result.id        = id;
    result.name      = QString::fromLatin1(name);
    result.clockrate = clockrate;
    result.channels  = channels;
    return result;
}

RtpGroupBridge::Endpoint endpoint(const QByteArray &id, const QString &media, const PPayloadInfo &payload,
                                  quint32 incomingSsrc, quint32 localSsrc)
{
    RtpGroupBridge::Endpoint result;
    result.id               = id;
    result.media            = media;
    result.localPayloads    = { payload };
    result.remotePayloads   = { payload };
    result.route.endpointId = id;
    result.route.incomingPayloadTypes.insert(quint8(payload.id));
    result.route.incomingSsrcs.insert(incomingSsrc);
    result.route.localSsrcs.insert(localSsrc);
    return result;
}

struct ProfileSizes {
    int key;
    int salt;
};

ProfileSizes sizesFor(const QString &profile)
{
    if (profile == QLatin1String("SRTP_AES128_CM_HMAC_SHA1_80")
        || profile == QLatin1String("SRTP_AES128_CM_HMAC_SHA1_32"))
        return { 16, 14 };
    if (profile == QLatin1String("SRTP_AEAD_AES_128_GCM"))
        return { 16, 12 };
    if (profile == QLatin1String("SRTP_AEAD_AES_256_GCM"))
        return { 32, 12 };
    qFatal("unexpected SRTP profile");
}

bool isRtp(const PSecureRtpPacket &packet) { return packet.type == PRtpPacket::Type::Rtp; }
bool isRtcp(const PSecureRtpPacket &packet) { return packet.type == PRtpPacket::Type::Rtcp; }

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    {
        GstRtpSessionContext legacy(nullptr, nullptr);
        check(qobject_cast<RtpSessionContext *>(legacy.qobject()) != nullptr,
              "legacy session lost RtpSessionContext/1.6");
        check(qobject_cast<SecureRtpSessionContext *>(legacy.qobject()) == nullptr,
              "legacy session falsely advertised secure RTP IID");

        GstSecureRtpSessionContext secure(nullptr, nullptr);
        check(qobject_cast<RtpSessionContext *>(secure.qobject()) != nullptr, "secure session lost legacy media IID");
        check(qobject_cast<SecureRtpSessionContext *>(secure.qobject()) != nullptr,
              "secure session did not advertise secure RTP IID");
    }

    const auto profiles = SrtpAssociation::supportedProfiles();
    check(!profiles.isEmpty(), "no usable SRTP profile for secure group test");
    const QString profile = profiles.contains(QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80"))
        ? QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80")
        : profiles.constFirst();
    const auto    sizes   = sizesFor(profile);

    // Public secure media sessions must represent unbundled audio/video as two
    // independent associations while keeping one codec/device session object.
    {
        GstSecureRtpSessionContext session(nullptr, nullptr);
        SecureRtpSessionContext   *secure = &session;

        PSecureRtpEndpoint audio;
        audio.endpointId           = QByteArrayLiteral("audio");
        audio.associationId        = QByteArrayLiteral("audio-association");
        audio.media                = QStringLiteral("audio");
        audio.incomingPayloadTypes = { 111 };

        PSecureRtpEndpoint video;
        video.endpointId           = QByteArrayLiteral("video");
        video.associationId        = QByteArrayLiteral("video-association");
        video.media                = QStringLiteral("video");
        video.incomingPayloadTypes = { 96 };

        check(secure->configureEndpoints({ audio, video }), "unbundled secure endpoint map rejected");

        const QByteArray audioLocalKey(sizes.key, char(0x51));
        const QByteArray audioLocalSalt(sizes.salt, char(0x52));
        const QByteArray audioRemoteKey(sizes.key, char(0x53));
        const QByteArray audioRemoteSalt(sizes.salt, char(0x54));
        const QByteArray videoLocalKey(sizes.key, char(0x61));
        const QByteArray videoLocalSalt(sizes.salt, char(0x62));
        const QByteArray videoRemoteKey(sizes.key, char(0x63));
        const QByteArray videoRemoteSalt(sizes.salt, char(0x64));

        check(secure->configureAssociation(audio.associationId, 3, profile, audioLocalKey, audioLocalSalt,
                                           audioRemoteKey, audioRemoteSalt),
              "audio secure association activation failed");
        check(secure->configureAssociation(video.associationId, 7, profile, videoLocalKey, videoLocalSalt,
                                           videoRemoteKey, videoRemoteSalt),
              "video secure association activation failed");
        check(secure->associationReady(audio.associationId) && secure->associationEpoch(audio.associationId) == 3,
              "audio association state missing");
        check(secure->associationReady(video.associationId) && secure->associationEpoch(video.associationId) == 7,
              "video association state missing");

        secure->invalidateAssociation(video.associationId, 7);
        check(!secure->associationReady(video.associationId), "video invalidation did not clear video crypto");
        check(secure->associationReady(audio.associationId) && secure->associationEpoch(audio.associationId) == 3,
              "video invalidation disturbed independent audio association");

        check(secure->configureEndpoints({}), "clearing all secure endpoint routes failed");
        check(secure->associationReady(audio.associationId) && secure->associationEpoch(audio.associationId) == 3,
              "route teardown destroyed staged audio crypto state");

        secure->invalidateAssociation(audio.associationId, 3);
        check(!secure->associationReady(audio.associationId), "audio invalidation did not clear audio crypto");
    }

    const QByteArray aKey(sizes.key, char(0x11));
    const QByteArray aSalt(sizes.salt, char(0x22));
    const QByteArray bKey(sizes.key, char(0x33));
    const QByteArray bSalt(sizes.salt, char(0x44));
    const QByteArray associationId = QByteArrayLiteral("bundle-association");

    constexpr quint32 AAudio = 0x10101010;
    constexpr quint32 AVideo = 0x20202020;
    constexpr quint32 BAudio = 0x30303030;
    constexpr quint32 BVideo = 0x40404040;

    const auto opus = payload(111, "OPUS", 48000, 2);
    const auto vp8  = payload(96, "VP8", 90000);

    SecureRtpGroup a;
    SecureRtpGroup b;
    check(a.isValid() && b.isValid(), "failed to construct secure RTP groups");

    const auto aAudio = endpoint(QByteArrayLiteral("audio"), QStringLiteral("audio"), opus, BAudio, AAudio);
    const auto aVideo = endpoint(QByteArrayLiteral("video"), QStringLiteral("video"), vp8, BVideo, AVideo);
    const auto bAudio = endpoint(QByteArrayLiteral("audio"), QStringLiteral("audio"), opus, AAudio, BAudio);
    const auto bVideo = endpoint(QByteArrayLiteral("video"), QStringLiteral("video"), vp8, AVideo, BVideo);
    check(a.configureEndpoints({ aAudio, aVideo }), "peer A endpoint configuration failed");
    check(b.configureEndpoints({ bAudio, bVideo }), "peer B endpoint configuration failed");

    check(a.activate(associationId, 1, profile, aKey, aSalt, bKey, bSalt), "peer A SRTP activation failed");
    check(b.activate(associationId, 1, profile, bKey, bSalt, aKey, aSalt), "peer B SRTP activation failed");

    std::vector<PSecureRtpPacket>               aNetwork;
    std::vector<PSecureRtpPacket>               bNetwork;
    int                                         bAudioDeliveries = 0;
    int                                         bVideoDeliveries = 0;
    std::vector<SecureRtpSessionContext::Error> aFatal;
    std::vector<SecureRtpSessionContext::Error> bFatal;

    a.setProtectedPacketHandler([&](const PSecureRtpPacket &packet) { aNetwork.push_back(packet); });
    b.setProtectedPacketHandler([&](const PSecureRtpPacket &packet) { bNetwork.push_back(packet); });
    b.setEndpointMediaPacketHandler(QByteArrayLiteral("audio"), [&](GstBuffer *) { ++bAudioDeliveries; });
    b.setEndpointMediaPacketHandler(QByteArrayLiteral("video"), [&](GstBuffer *) { ++bVideoDeliveries; });
    a.setRuntimeErrorHandler([&](auto error) { aFatal.push_back(error); });
    b.setRuntimeErrorHandler([&](auto error) { bFatal.push_back(error); });
    a.setRtcpMinimumInterval(10 * GST_MSECOND);
    b.setRtcpMinimumInterval(10 * GST_MSECOND);

    check(a.start() && b.start(), "failed to start secure groups");

    // Send enough consecutive RTP for the remote shared rtpsession probation.
    for (quint16 sequence = 1; sequence <= 3; ++sequence) {
        const QByteArray bytes  = makeRtp(111, sequence, quint32(sequence) * 960, AAudio);
        GstBuffer       *buffer = bufferFor(bytes, quint64(sequence) * 20 * GST_MSECOND);
        check(a.sendRtp(QByteArrayLiteral("audio"), buffer, 0) == GST_FLOW_OK, "secure audio send failed");
        gst_buffer_unref(buffer);
    }
    for (quint16 sequence = 1; sequence <= 3; ++sequence) {
        const QByteArray bytes  = makeRtp(96, sequence, quint32(sequence) * 3000, AVideo);
        GstBuffer       *buffer = bufferFor(bytes, quint64(sequence) * 33 * GST_MSECOND);
        check(a.sendRtp(QByteArrayLiteral("video"), buffer, 0) == GST_FLOW_OK, "secure video send failed");
        gst_buffer_unref(buffer);
    }

    check(waitUntil([&] { return std::count_if(aNetwork.cbegin(), aNetwork.cend(), isRtp) >= 6; }),
          "outgoing RTP was not protected");

    size_t deliveredToB = 0;
    for (const auto &packet : aNetwork) {
        if (!isRtp(packet))
            continue;
        check(b.receiveProtectedPacket(packet), "peer B rejected authenticated SRTP");
        ++deliveredToB;
    }
    check(deliveredToB >= 6, "not enough SRTP reached peer B");
    check(waitUntil([&] { return bAudioDeliveries > 0 && bVideoDeliveries > 0; }),
          "authenticated audio/video did not reach endpoint media handlers");

    // Authentication failure is a packet drop, not a fatal backend failure, and
    // must not consume the packet index or teach an RTP route.
    const size_t     beforeTamper    = aNetwork.size();
    const auto       beforeTamperRtp = std::count_if(aNetwork.cbegin(), aNetwork.cend(), isRtp);
    const QByteArray freshBytes      = makeRtp(111, 10, 9600, AAudio);
    PRtpPacket       fresh;
    fresh.type     = PRtpPacket::Type::Rtp;
    fresh.rawValue = freshBytes;
    check(a.sendRtp(QByteArrayLiteral("audio"), fresh) == GST_FLOW_OK, "fresh SRTP send failed");
    check(waitUntil([&] { return std::count_if(aNetwork.cbegin(), aNetwork.cend(), isRtp) > beforeTamperRtp; }),
          "fresh SRTP packet was not protected");

    const auto freshIt = std::find_if(aNetwork.cbegin() + std::ptrdiff_t(beforeTamper), aNetwork.cend(), isRtp);
    check(freshIt != aNetwork.cend(), "fresh protected RTP packet missing");
    auto freshProtected = *freshIt;
    auto tampered       = freshProtected;
    tampered.rawValue[tampered.rawValue.size() - 1]
        = char(quint8(tampered.rawValue.at(tampered.rawValue.size() - 1)) ^ 0x01);
    check(!b.receiveProtectedPacket(tampered), "tampered SRTP was accepted");
    check(b.lastError() == SecureRtpSessionContext::Error::Authentication,
          "tampered SRTP did not report authentication failure");
    check(bFatal.empty(), "authentication drop was promoted to fatal backend failure");
    check(b.receiveProtectedPacket(freshProtected), "auth failure poisoned the valid packet/replay state");

    // An authenticated packet that does not belong to any negotiated media
    // route is a nonfatal media-layer drop. It must not be reported as a
    // cryptographic failure and must not teach the router.
    SrtpAssociation rogueSender;
    check(rogueSender.configure(associationId, 1, profile, aKey, aSalt, bKey, bSalt), "rogue sender SRTP setup failed");
    PSecureRtpPacket roguePlain;
    roguePlain.associationId = associationId;
    roguePlain.epoch         = 1;
    roguePlain.type          = PRtpPacket::Type::Rtp;
    roguePlain.rawValue      = makeRtp(127, 1, 12345, 0x51515151);
    PSecureRtpPacket rogueProtected;
    check(rogueSender.protect(roguePlain, &rogueProtected), "rogue RTP protection failed");
    check(!b.receiveProtectedPacket(rogueProtected), "authenticated unknown-route RTP was accepted");
    check(b.lastError() == SecureRtpSessionContext::Error::InvalidPacket,
          "authenticated route drop did not report InvalidPacket");
    check(bFatal.empty(), "authenticated route drop was promoted to fatal backend failure");

    // Group RTCP generated from the same RFC 3550 session is protected as SRTCP
    // once. The peer authenticates it before feeding the group session once.
    check(waitUntil([&] { return a.requestRtcp(100 * GST_MSECOND); }), "shared RTCP scheduler did not become ready");
    check(waitUntil([&] { return std::any_of(aNetwork.cbegin(), aNetwork.cend(), isRtcp); }),
          "outgoing group RTCP was not protected");

    const auto rtcpIt = std::find_if(aNetwork.cbegin(), aNetwork.cend(), isRtcp);
    check(rtcpIt != aNetwork.cend(), "protected RTCP packet missing");
    const auto beforeRtcp = b.receivedRtcpPackets();
    check(b.receiveProtectedPacket(*rtcpIt), "authenticated SRTCP was rejected");
    check(waitUntil([&] { return b.receivedRtcpPackets() == beforeRtcp + 1; }),
          "SRTCP was not consumed exactly once by peer group session");

    // Identical-key activation advances the external epoch while retaining
    // libSRTP replay/ROC/SRTCP state. Old metadata is fenced immediately.
    check(a.activate(associationId, 2, profile, aKey, aSalt, bKey, bSalt), "peer A epoch advance failed");
    check(b.activate(associationId, 2, profile, bKey, bSalt, aKey, aSalt), "peer B epoch advance failed");
    check(a.isReady() && b.isReady() && a.epoch() == 2 && b.epoch() == 2, "epoch advance lost crypto state");
    check(!b.receiveProtectedPacket(freshProtected), "old-epoch protected packet crossed reactivation");
    check(b.lastError() == SecureRtpSessionContext::Error::StaleEpoch, "old epoch was not reported as stale");

    // Endpoint membership changes do not recreate the secure association.
    check(a.removeEndpoint(QByteArrayLiteral("video")), "peer A video removal failed");
    check(b.removeEndpoint(QByteArrayLiteral("video")), "peer B video removal failed");
    check(a.isReady() && b.isReady() && a.epoch() == 2 && b.epoch() == 2, "endpoint removal reset secure association");

    const size_t beforeFinalAudio = aNetwork.size();
    const auto   beforeFinalRtp   = std::count_if(aNetwork.cbegin(), aNetwork.cend(), isRtp);
    PRtpPacket   finalAudio;
    finalAudio.type     = PRtpPacket::Type::Rtp;
    finalAudio.rawValue = makeRtp(111, 11, 10560, AAudio);
    check(a.sendRtp(QByteArrayLiteral("audio"), finalAudio) == GST_FLOW_OK,
          "surviving audio failed after video removal");
    check(waitUntil([&] { return std::count_if(aNetwork.cbegin(), aNetwork.cend(), isRtp) > beforeFinalRtp; }),
          "surviving audio was not protected after video removal");
    const auto finalIt = std::find_if(aNetwork.cbegin() + std::ptrdiff_t(beforeFinalAudio), aNetwork.cend(), isRtp);
    check(finalIt != aNetwork.cend(), "final protected RTP packet missing");
    auto finalProtected = *finalIt;
    check(finalProtected.epoch == 2, "final audio used wrong secure epoch");
    check(b.receiveProtectedPacket(finalProtected), "peer B rejected surviving audio after video removal");

    a.stop();
    b.stop();
    check(a.isReady() && b.isReady(), "media stop invalidated secure association");
    a.invalidate(associationId, 2);
    b.invalidate(associationId, 2);
    check(!a.isReady() && !b.isReady(), "explicit security invalidation did not clear associations");
    check(aFatal.empty() && bFatal.empty(), "unexpected fatal secure group error");

    qInfo("secure RTP group end-to-end regression passed");
    return 0;
}
