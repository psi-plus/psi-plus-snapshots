// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "sxeconnectionfixture.h"
#include <iris/dtls.h>
#include <iris/jingle-rtp.h>

namespace R = J::RTP;

class AudioEndpoint : public R::MediaEndpoint {
public:
    R::Description localOffer() const override
    {
        R::Description offer;
        offer.media   = "audio";
        offer.rtcpMux = true;
        R::PayloadType payload;
        payload.id        = 96;
        payload.name      = "opus";
        payload.clockrate = 48000;
        payload.channels  = 2;
        offer.payloads.append(payload);
        return offer;
    }
    std::optional<R::Description> makeAnswer(const R::Description &offer) const override { return offer; }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &, const R::Description &) override { return true; }
    void stop() override { }
};

class VideoEndpoint : public AudioEndpoint {
public:
    R::Description localOffer() const override
    {
        auto offer                  = AudioEndpoint::localOffer();
        offer.media                 = "video";
        offer.payloads[0].name      = "VP8";
        offer.payloads[0].clockrate = 90000;
        offer.payloads[0].channels  = 1;
        return offer;
    }
};
// Codec/capture are fixtures; signaling, ICE, DTLS and BUNDLE are production.
class RaceSession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        if (media == "video")
            return std::make_unique<VideoEndpoint>();
        return std::make_unique<AudioEndpoint>();
    }
    bool configureSecureRtpEndpoints(const QList<R::SecureRtpEndpoint> &) override { return true; }
    bool configureSecureRtpAssociation(const R::SecureRtpParameters &) override { return true; }
    bool attachSecureRtpPacketIo(ProtectedPacketWriter) override { return true; }
};
class RaceProvider : public R::MediaProvider {
public:
    std::unique_ptr<R::MediaSession> createSession() override { return std::make_unique<RaceSession>(); }
    QStringList                      mediaTypes() const override { return { "audio", "video" }; }
    QStringList                      secureRtpProfiles() const override { return { "SRTP_AES128_CM_HMAC_SHA1_80" }; }
};
