// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QtCrypto>

#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/xmpp-im/jingle-s5b.h>
#include <iris/jingle-ice.h>
#include <iris/jingle-rtp.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>

using namespace XMPP;
namespace J = XMPP::Jingle;
static const QString DtlsFeature = QStringLiteral("urn:xmpp:jingle:apps:dtls:0");

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Endpoint final : public J::RTP::MediaEndpoint {
public:
    J::RTP::Description localOffer() const override
    {
        J::RTP::Description description;
        description.media   = QStringLiteral("audio");
        description.rtcpMux = true;
        J::RTP::PayloadType payload;
        payload.id        = 96;
        payload.name      = QStringLiteral("opus");
        payload.clockrate = 48000;
        payload.channels  = 2;
        description.payloads.append(payload);
        return description;
    }

    std::optional<J::RTP::Description> makeAnswer(const J::RTP::Description &offer) const override
    {
        return offer.media == QLatin1String("audio") && offer.rtcpMux ? std::optional(offer) : std::nullopt;
    }

    bool acceptsAnswer(const J::RTP::Description &, const J::RTP::Description &) const override { return true; }
    bool configure(const J::RTP::Description &, const J::RTP::Description &) override { return true; }
    void stop() override { }
};

class MediaSession final : public J::RTP::MediaSession {
public:
    std::unique_ptr<J::RTP::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return media == QLatin1String("audio") ? std::make_unique<Endpoint>() : nullptr;
    }
};

class Provider final : public J::RTP::MediaProvider {
public:
    std::unique_ptr<J::RTP::MediaSession> createSession() override { return std::make_unique<MediaSession>(); }
    QStringList mediaTypes() const override { return { QStringLiteral("audio") }; }
    QStringList secureRtpProfiles() const override
    {
        return { QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80") };
    }
};

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:rtp-caps-selection"));
    disco.setFeatures(Features(features));

    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

static QStringList rtpFeatures(J::RTP::Manager *rtp)
{
    const auto features = rtp->discoFeatures();
    check(features.contains(J::RTP::Description::ns()), "RTP description capability was not advertised");
    check(features.contains(QStringLiteral("urn:xmpp:jingle:apps:rtp:audio")),
          "audio RTP capability was not advertised");
    check(features.contains(QStringLiteral("urn:xmpp:jingle:apps:rtp:rtcp-fb:0")),
          "RTCP feedback negotiation capability was not advertised");
    return features;
}

static QStringList secureAudioProfile(J::RTP::Manager *rtp)
{
    QStringList features { J::NS, DtlsFeature };
    features += rtpFeatures(rtp);
    features.removeDuplicates();
    return features;
}

static void localAdvertisement()
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    check(rtp->discoFeatures().isEmpty(), "RTP was advertised without a media provider");

    rtp->setMediaProvider(std::make_shared<Provider>());
    check(rtp->discoFeatures().isEmpty(), "RTP was advertised without an enabled transport");

    rtp->setTransportNamespaces({ J::IBB::NS });
    check(rtp->discoFeatures().isEmpty(), "RTP was advertised over a byte-stream-only transport");

    rtp->setTransportNamespaces({ J::ICE::NS });
    const auto advertised = rtp->discoFeatures();
    if (advertised.isEmpty())
        return; // Build/runtime has no usable DTLS-SRTP profile.

    const auto features = rtpFeatures(rtp);
    check(client.jingleManager()->discoFeatures().contains(J::NS),
          "Jingle manager did not advertise the generic Jingle capability");
    check(client.makeDiscoResult().features().test(J::NS),
          "Client disco omitted the Jingle manager's generic capability");
    check(client.jingleICEManager()->discoFeatures().contains(DtlsFeature),
          "secure RTP was advertised while the ICE manager omitted DTLS");
    check(client.jingleManager()->discoFeatures().contains(QStringLiteral("urn:ietf:rfc:5888")),
          "grouping capability was not advertised");
}

static void peerProfileRequirements()
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });
    if (rtp->discoFeatures().isEmpty())
        return;

    const Jid peer(QStringLiteral("profile-peer@example.test/device"));
    auto full = secureAudioProfile(rtp);
    full += J::ICE::NS;

    auto canCreate = [&](QStringList features, bool expectGrouping = false) {
        setPeerFeatures(client, peer, std::move(features));
        J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
        auto app = dynamic_cast<J::RTP::Application *>(
            rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
        if (app)
            check(session.isGroupingAllowed() == expectGrouping,
                  "peer grouping capability did not control BUNDLE eligibility");
        return app != nullptr;
    };

    check(canCreate(full), "complete secure RTP peer profile was rejected");

    auto withGrouping = full;
    withGrouping += QStringLiteral("urn:ietf:rfc:5888");
    check(canCreate(withGrouping, true), "grouping-capable secure RTP peer was rejected");

    auto missing = full;
    missing.removeAll(J::NS);
    check(!canCreate(missing), "RTP application accepted peer without Jingle capability");

    missing = full;
    missing.removeAll(J::RTP::Description::ns());
    check(!canCreate(missing), "RTP application accepted peer without RTP description capability");

    missing = full;
    missing.removeAll(QStringLiteral("urn:xmpp:jingle:apps:rtp:audio"));
    check(!canCreate(missing), "RTP application accepted peer without audio RTP capability");

    missing = full;
    missing.removeAll(DtlsFeature);
    check(!canCreate(missing), "RTP application accepted peer without DTLS capability");
}

static void validIceSelection()
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });
    if (rtp->discoFeatures().isEmpty())
        return;

    const Jid peer(QStringLiteral("ice-peer@example.test/device"));
    QStringList caps = secureAudioProfile(rtp);
    caps += client.jingleICEManager()->discoFeatures();
    setPeerFeatures(client, peer, caps);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto app = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    check(app, "failed to create RTP application for ICE caps");
    check(app->selectNextTransport(), "RTP application rejected advertised ICE transport");
    check(app->transport() && app->transport()->pad()->ns() == J::ICE::NS,
          "RTP application selected the wrong transport for ICE caps");
}

static void replacementDisabledAfterConnecting()
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    // Keep a second compatible ICE namespace available so selectNextTransport()
    // would return the same unconsumed candidate forever if setTransport()
    // rejected replacement after Connecting.
    rtp->setTransportNamespaces({ J::ICE::NS, J::ICE::NS_ICE_UDP });
    if (rtp->discoFeatures().isEmpty())
        return;

    const Jid peer(QStringLiteral("connecting-fallback-peer@example.test/device"));
    QStringList caps = secureAudioProfile(rtp);
    caps += client.jingleICEManager()->discoFeatures();
    setPeerFeatures(client, peer, caps);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto app = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    check(app, "failed to create RTP application for connecting fallback regression");
    check(app->selectNextTransport(), "failed to install initial RTP transport");
    check(app->transport() && app->transport()->pad()->ns() == J::ICE::NS_ICE_UDP,
          "fixture did not leave the alternate ICE namespace available");

    const auto first = app->transport();
    app->setState(J::State::Connecting);
    check(app->selectNextTransport(), "RTP did not fall back after pre-connected transport failure");
    check(app->transport() && app->transport() != first
              && app->transport()->pad()->ns() == J::ICE::NS,
          "RTP fallback did not select the remaining ICE transport");
    check(app->state() == J::State::Connecting,
          "pre-connected RTP fallback incorrectly terminated the application");
}

static void incompatibleTransportCaps(const QStringList &extraCaps, const char *message)
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    // Current production RTP backend supports packet-oriented ICE only. Keep
    // the policy explicit rather than teaching the generic selector that IBB
    // can carry RTP merely because a future codec might have a tiny bitrate.
    rtp->setTransportNamespaces({ J::ICE::NS });
    if (rtp->discoFeatures().isEmpty())
        return;

    const Jid peer(QStringLiteral("non-ice-peer@example.test/device"));
    QStringList caps = secureAudioProfile(rtp);
    caps += extraCaps;
    setPeerFeatures(client, peer, caps);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto app = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    check(app, "failed to create RTP application for incompatible transport caps");
    check(!app->selectNextTransport(), message);
    check(!app->transport(), "RTP application installed an incompatible transport");
    check(app->state() == J::State::Finished,
          "RTP application did not fail closed after exhausting peer transport capabilities");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    localAdvertisement();
    peerProfileRequirements();
    validIceSelection();
    replacementDisabledAfterConnecting();

    {
        Client probe;
        incompatibleTransportCaps(probe.jingleIBBManager()->discoFeatures(),
                                  "RTP application accepted an IBB-only peer");
    }

    {
        Client probe;
        auto caps = probe.jingleIBBManager()->discoFeatures();
        caps += QStringLiteral("urn:ietf:rfc:5888");
        incompatibleTransportCaps(caps,
                                  "RTP application accepted IBB because the peer also advertised grouping");
    }

    {
        Client probe;
        incompatibleTransportCaps(probe.jingleS5BManager()->discoFeatures(),
                                  "RTP application accepted an S5B-only peer");
    }

    incompatibleTransportCaps({}, "RTP application accepted a peer with no compatible transport capability");

    qInfo("RTP capability/transport selection regressions passed");
    return 0;
}
