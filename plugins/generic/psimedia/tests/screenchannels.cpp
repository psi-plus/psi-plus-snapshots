// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gstprovider.h"
#include "gstrtpsessioncontext.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QtEndian>
#include <memory>
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
static void prepare(GstRtpSessionContext *codec)
{
    PVideoParams mode;
    mode.codec = QStringLiteral("vp8");
    mode.size  = QSize(320, 180);
    mode.fps   = 15;
    PPayloadInfo payload;
    payload.id        = 96;
    payload.name      = QStringLiteral("VP8");
    payload.clockrate = 90000;
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
    int     cameraPackets = 0, screenPackets = 0;
    quint32 cameraSsrc = 0;
    owner->setProtectedPacketHandler([&](const PSecureRtpPacket &packet) {
        if (packet.type != PRtpPacket::Type::Rtp || packet.rawValue.size() < 12)
            return;
        check(peerOwner->receiveProtectedPacket(packet), "peer rejected authenticated camera/screen RTP");
        const auto ssrc = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(packet.rawValue.constData()) + 8);
        if (!cameraSsrc)
            cameraSsrc = ssrc;
        if (ssrc == cameraSsrc)
            ++cameraPackets;
        else
            ++screenPackets;
    });
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
    prepare(peerScreen.get());
    prepare(screen.get());
    capture(screen.get(), "videotestsrc is-live=true pattern=smpte");
    const int cameraBefore = cameraPackets;
    waitFor([&] { return screenPackets >= 6 && cameraPackets >= cameraBefore + 6; },
            "camera and screen did not transmit concurrently");
    check(owner->associationEpoch("bundle") == 1, "extension replaced the crypto epoch");
    screen->pauseVideo();
    check(screen->configureEndpoints({}), "screen route removal failed");
    screen.reset();
    const int remainingBefore = cameraPackets;
    waitFor([&] { return cameraPackets >= remainingBefore + 6; }, "camera stopped with screen");
    check(owner->associationEpoch("bundle") == 1, "screen removal replaced the crypto epoch");
    owner->setProtectedPacketHandler({});
    qInfo("Concurrent camera/screen capture and independent teardown passed");
}
