// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gstprovider.h"
#include "gstrtpsessioncontext.h"
#include "rwcontrol.h"
#include "screencapturedevice_p.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QtEndian>
#include <gst/rtp/gstrtpbuffer.h>
#include <memory>
#include <optional>
using namespace PsiMedia;
static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
template <class F> static void waitFor(F condition, const char *message)
{
    QElapsedTimer time;
    time.start();
    while (!condition() && time.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    check(condition(), message);
}
static void prepare(GstRtpSessionContext *codec, const QSize &size = QSize(320, 180))
{
    PVideoParams mode;
    mode.codec = QStringLiteral("vp8");
    mode.size  = size;
    mode.fps   = 15;
    PPayloadInfo payload;
    payload.id        = 96;
    payload.name      = QStringLiteral("VP8");
    payload.clockrate = 90000;
    PPayloadInfo::Parameter pli;
    pli.name  = QStringLiteral("rtcp-fb-nack-pli");
    pli.value = QStringLiteral("true");
    payload.parameters.append(pli);
    codec->setLocalVideoPreferences({ mode });
    codec->setRemoteVideoPreferences({ payload });
    bool ready      = false;
    auto connection = QObject::connect(codec, &GstRtpSessionContext::started, [&] { ready = true; });
    codec->start();
    waitFor([&] { return ready; }, "codec preparation timed out");
    QObject::disconnect(connection);
}
static void capture(GstRtpSessionContext *codec, const QString &source)
{
    bool ready      = false;
    auto connection = QObject::connect(codec, &GstRtpSessionContext::preferencesUpdated, [&] { ready = true; });
    codec->setVideoInputDevice(source);
    codec->updatePreferences();
    waitFor([&] { return ready; }, "capture update timed out");
    QObject::disconnect(connection);
    codec->transmitVideo();
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const bool       hd           = app.arguments().contains(QStringLiteral("--hd"));
    const bool       loss         = app.arguments().contains(QStringLiteral("--loss"));
    const bool       switchDevice = app.arguments().contains(QStringLiteral("--switch"));
    GstProvider      provider;
    check(provider.isInitialized(), "GStreamer provider initialization failed");
    const auto create = [&] {
        auto result = std::unique_ptr<GstSecureRtpSessionContext>(
            qobject_cast<GstSecureRtpSessionContext *>(provider.createSecureRtpSession()->qobject()));
        check(bool(result), "secure codec unavailable");
        QObject::connect(result.get(), &GstRtpSessionContext::error, [] { qFatal("codec runtime error"); });
        return result;
    };
    auto owner  = create();
    auto camera = create();
    check(camera->shareSecureGroupsWith(owner.get()), "camera failed to join owner");
    PSecureRtpEndpoint cameraRoute;
    cameraRoute.endpointId           = "camera";
    cameraRoute.mid                  = "camera";
    cameraRoute.midExtensionId       = 1;
    cameraRoute.associationId        = "bundle";
    cameraRoute.media                = "video";
    cameraRoute.incomingPayloadTypes = { 96 };
    check(camera->configureEndpoints({ cameraRoute }), "camera route failed");
    prepare(camera.get());
    check(owner->configureAssociation("bundle", 1, "SRTP_AES128_CM_HMAC_SHA1_80", QByteArray(16, 'a'),
                                      QByteArray(14, 'b'), QByteArray(16, 'c'), QByteArray(14, 'd')),
          "crypto setup failed");
    auto peerOwner  = create();
    auto peerCamera = create();
    check(peerCamera->shareSecureGroupsWith(peerOwner.get()), "peer camera owner failed");
    check(peerCamera->configureEndpoints({ cameraRoute }), "peer camera route failed");
    prepare(peerCamera.get());
    check(peerOwner->configureAssociation("bundle", 1, "SRTP_AES128_CM_HMAC_SHA1_80", QByteArray(16, 'c'),
                                          QByteArray(14, 'd'), QByteArray(16, 'a'), QByteArray(14, 'b')),
          "peer crypto failed");
    int                    cameraPackets = 0, screenPackets = 0;
    int                    cameraFrames = 0, screenFrames = 0;
    std::optional<quint32> lostTimestamp;
    int                    framesAtLoss = 0;
    QElapsedTimer          recoveryTime;
    qint64                 recoveryMs = -1;
    owner->setProtectedPacketHandler([&](const PSecureRtpPacket &packet) {
        if (packet.type != PRtpPacket::Type::Rtp || packet.rawValue.size() < 12) {
            check(peerOwner->receiveProtectedPacket(packet), "peer rejected sender RTCP");
            return;
        }
        // Sender replacement may change SSRC. Route by the stable MID,
        // which is present in the clear RTP header even for protected packets.
        GstBuffer   *buffer = gst_rtp_buffer_new_copy_data(packet.rawValue.constData(), packet.rawValue.size());
        GstRTPBuffer rtp    = GST_RTP_BUFFER_INIT;
        check(gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp), "invalid protected RTP header");
        gpointer value = nullptr;
        guint    size  = 0;
        check(gst_rtp_buffer_get_extension_onebyte_header(&rtp, 1, 0, &value, &size), "missing MID");
        const QByteArray mid(static_cast<const char *>(value), size);
        gst_rtp_buffer_unmap(&rtp);
        gst_buffer_unref(buffer);
        if (mid == "camera")
            ++cameraPackets;
        else {
            check(mid == "screen", "unexpected MID");
            ++screenPackets;
            const auto timestamp
                = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(packet.rawValue.constData()) + 4);
            if (loss && !lostTimestamp && screenFrames >= 10) {
                lostTimestamp = timestamp;
                framesAtLoss  = screenFrames;
                recoveryTime.start();
            }
            if (lostTimestamp && *lostTimestamp == timestamp)
                return; // Drop precisely one whole encrypted screen frame at the network edge.
        }
        check(peerOwner->receiveProtectedPacket(packet), "peer rejected authenticated camera/screen RTP");
    });
    peerOwner->setProtectedPacketHandler([&](const PSecureRtpPacket &packet) {
        check(owner->receiveProtectedPacket(packet), "sender rejected peer RTCP feedback");
    });
    auto cameraDecoded
        = QObject::connect(peerCamera->control, &RwControlLocal::outputFrame, [&](const QImage &) { ++cameraFrames; });
    capture(camera.get(), "videotestsrc is-live=true pattern=ball");
    waitFor([&] { return cameraPackets >= 6; }, "camera produced no protected RTP");
    auto screen = create();
    check(screen->shareSecureGroupsWith(owner.get()), "screen failed to join live owner");
    auto screenRoute       = cameraRoute;
    screenRoute.endpointId = "screen";
    screenRoute.mid        = "screen";
    check(screen->configureEndpoints({ screenRoute }), "screen route extension failed");
    auto peerScreen = create();
    check(peerScreen->shareSecureGroupsWith(peerOwner.get()) && peerScreen->configureEndpoints({ screenRoute }),
          "peer screen route extension failed");
    const QSize screenSize = hd ? QSize(1920, 1080) : QSize(320, 180);
    prepare(peerScreen.get(), screenSize);
    screen->setMaximumSendingBitrate(2000);
    prepare(screen.get(), screenSize);
    QElapsedTimer decodedTime;
    decodedTime.start();
    qint64 lastFrame = 0, maxGap = 0;
    auto   screenDecoded = QObject::connect(peerScreen->control, &RwControlLocal::outputFrame, [&](const QImage &) {
        const auto now = decodedTime.elapsed();
        if (screenFrames)
            maxGap = qMax(maxGap, now - lastFrame);
        lastFrame = now;
        ++screenFrames;
        // Allow older frames already in the receive jitterbuffer to drain;
        // recovery requires several additional decoded frames after the loss.
        if (lostTimestamp && recoveryMs < 0 && screenFrames >= framesAtLoss + 6)
            recoveryMs = recoveryTime.elapsed();
    });
    auto   screenDevice  = registerScreenCaptureDevice(QStringLiteral("videotestsrc is-live=true pattern=smpte"));
    capture(screen.get(), screenDevice->id);
    const int cameraBefore = cameraPackets;
    waitFor([&] { return screenPackets >= 6 && cameraPackets >= cameraBefore + 6; },
            "camera and screen did not transmit concurrently");
    waitFor([&] { return screenFrames >= 30 && cameraFrames >= 30; },
            "camera/screen RTP arrived but did not produce decoded frames");
    qInfo("Screen decoded=%d camera decoded=%d elapsed-ms=%lld max-screen-gap-ms=%lld hd=%d", screenFrames,
          cameraFrames, (long long)decodedTime.elapsed(), (long long)maxGap, int(hd));
    if (loss) {
        qInfo("Screen loss recovery-ms=%lld", (long long)recoveryMs);
        check(lostTimestamp && recoveryMs >= 0 && recoveryMs < 3000,
              "screen loss did not recover through RTCP feedback before the periodic keyframe");
    }
    if (switchDevice) {
        const int beforeCamera = cameraFrames, beforeScreen = screenFrames;
        // Replace an active camera, without toggling transmit or rebuilding
        // the shared RTP owner. The independent screen must keep decoding.
        camera->setVideoInputDevice(QStringLiteral("videotestsrc is-live=true pattern=checkers-8"));
        waitFor([&] { return cameraFrames >= beforeCamera + 15 && screenFrames >= beforeScreen + 15; },
                "camera source switch interrupted camera or screen decoding");
        check(owner->associationEpoch("bundle") == 1, "camera switch replaced the crypto epoch");
    }
    QObject::disconnect(cameraDecoded);
    QObject::disconnect(screenDecoded);
    check(owner->associationEpoch("bundle") == 1, "extension replaced the crypto epoch");
    revokeScreenCaptureDevice(screenDevice->id);
    screenDevice.reset();
    screen->pauseVideo();
    check(screen->configureEndpoints({}), "screen route removal failed");
    screen.reset();
    const int remainingBefore = cameraPackets;
    waitFor([&] { return cameraPackets >= remainingBefore + 6; }, "camera stopped with screen");
    check(owner->associationEpoch("bundle") == 1, "screen removal replaced the crypto epoch");
    owner->setProtectedPacketHandler({});
    peerOwner->setProtectedPacketHandler({});
    qInfo("Concurrent camera/screen capture and independent teardown passed");
}
