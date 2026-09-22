#include <iris/dtls.h>
#include <iris/jingle-rtp-srtp.h>
#ifdef IRIS_TEST_SCTP
#include <iris/jingle-sctp.h>
#endif
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QEventLoop>
#include <QTimer>

using XMPP::Dtls;
using XMPP::Jingle::RTP::PacketKind;
using XMPP::Jingle::RTP::SecureRtpAssociation;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

#if QCA_MAJOR_VERSION >= 3
#ifdef IRIS_TEST_SCTP
static void runDataChannel(Dtls &first, Dtls &second)
{
    using namespace XMPP::Jingle;
    SCTP::Association sender(nullptr), receiver(nullptr);
    sender.setIdSelector(first.localFingerprint().setup == Dtls::Active ? SCTP::IdSelector::Even
                                                                        : SCTP::IdSelector::Odd);
    receiver.setIdSelector(second.localFingerprint().setup == Dtls::Active ? SCTP::IdSelector::Even
                                                                           : SCTP::IdSelector::Odd);

    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // This is the post-DTLS boundary used by jingle-ice.cpp. The encrypted
    // records themselves are transported through SecureRtpAssociation in runPair.
    auto wire = [](SCTP::Association &association, Dtls &dtls) {
        QObject::connect(&association, &SCTP::Association::readyReadOutgoing, &dtls, [&association, &dtls]() {
            while (association.pendingOutgoingDatagrams())
                dtls.writeDatagram(association.readOutgoing());
        });
        QObject::connect(&dtls, &Dtls::readyRead, &association, [&association, &dtls]() {
            for (auto data = dtls.readDatagram(); !data.isEmpty(); data = dtls.readDatagram())
                association.writeIncoming(data);
        });
    };
    wire(sender, first);
    wire(receiver, second);

    const QByteArray payload(32768, 'x');
    auto             channel = sender.newChannel();
    check(bool(channel), "SCTP channel creation failed");

    Connection::Ptr incoming;
    bool            received = false, echoed = false;
    QObject::connect(&receiver, &SCTP::Association::newIncomingChannel, &loop, [&]() {
        incoming = receiver.nextChannel();
        check(bool(incoming), "missing incoming SCTP channel");
        QObject::connect(incoming.data(), &QIODevice::readyRead, &loop, [&]() {
            while (incoming->hasPendingDatagrams()) {
                const auto packet = incoming->readDatagram();
                check(packet.data() == payload, "SCTP payload corrupted");
                received = true;
                check(incoming->writeDatagram(packet), "SCTP echo failed");
            }
        });
    });
    QObject::connect(channel.data(), &Connection::connected, &loop,
                     [&]() { check(channel->writeDatagram(QNetworkDatagram(payload)), "SCTP write failed"); });
    QObject::connect(channel.data(), &QIODevice::readyRead, &loop, [&]() {
        echoed = channel->readDatagram().data() == payload;
        loop.quit();
    });

    timer.start(5000);
    receiver.onTransportConnected();
    sender.onTransportConnected();
    loop.exec();
    check(received && echoed, "SCTP through secure RTP/DTLS mux failed or timed out");
}
#endif

static void runDeferredFirstFlight(const QCA::Certificate &cert, const QCA::PrivateKey &key)
{
    Dtls          passive;
    Dtls          active;
    const QString profile = QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80");
    const QStringList profiles { profile };

    passive.setNegotiationDeferred(true);
    active.setNegotiationDeferred(true);
    check(passive.setSRTPProfiles(profiles) && active.setSRTPProfiles(profiles),
          "pre-start buffering profiles rejected");
    passive.setLocalCertificate(cert, key);
    active.setLocalCertificate(cert, key);

    passive.initOutgoing();
    active.setRemoteFingerprint(passive.localFingerprint());
    active.acceptIncoming();
    passive.setRemoteFingerprint(active.localFingerprint());

    check(passive.localFingerprint().setup == Dtls::Passive && active.localFingerprint().setup == Dtls::Active,
          "unexpected DTLS roles in pre-start buffering regression");

    QEventLoop firstFlightLoop;
    QTimer     firstFlightTimer;
    firstFlightTimer.setSingleShot(true);
    QObject::connect(&firstFlightTimer, &QTimer::timeout, &firstFlightLoop, &QEventLoop::quit);

    bool earlyFlight = false;
    QObject::connect(&active, &Dtls::readyReadOutgoing, &firstFlightLoop, [&]() {
        for (auto packet = active.readOutgoingDatagram(); !packet.isEmpty(); packet = active.readOutgoingDatagram()) {
            if (!passive.isStarted())
                earlyFlight = true;
            passive.writeIncomingDatagram(packet);
        }
        if (earlyFlight && !passive.isStarted())
            firstFlightLoop.quit();
    });
    QObject::connect(&passive, &Dtls::readyReadOutgoing, &active, [&]() {
        for (auto packet = passive.readOutgoingDatagram(); !packet.isEmpty(); packet = passive.readOutgoingDatagram())
            active.writeIncomingDatagram(packet);
    });

    active.onRemoteAcceptedFingerprint();
    firstFlightTimer.start(1000);
    if (!earlyFlight)
        firstFlightLoop.exec();
    check(earlyFlight && !passive.isStarted(), "no DTLS client flight arrived before passive startup");

    QEventLoop handshakeLoop;
    QTimer     handshakeTimer;
    handshakeTimer.setSingleShot(true);
    QObject::connect(&handshakeTimer, &QTimer::timeout, &handshakeLoop, &QEventLoop::quit);

    bool passiveConnected = false;
    bool activeConnected  = false;
    bool failed           = false;
    auto maybeDone = [&]() {
        if (passiveConnected && activeConnected)
            handshakeLoop.quit();
    };
    QObject::connect(&passive, &Dtls::connected, &handshakeLoop, [&]() {
        passiveConnected = true;
        maybeDone();
    });
    QObject::connect(&active, &Dtls::connected, &handshakeLoop, [&]() {
        activeConnected = true;
        maybeDone();
    });
    QObject::connect(&passive, &Dtls::errorOccurred, &handshakeLoop, [&](QAbstractSocket::SocketError) {
        failed = true;
        handshakeLoop.quit();
    });
    QObject::connect(&active, &Dtls::errorOccurred, &handshakeLoop, [&](QAbstractSocket::SocketError) {
        failed = true;
        handshakeLoop.quit();
    });

    passive.onRemoteAcceptedFingerprint();
    handshakeTimer.start(5000);
    if (!(passiveConnected && activeConnected))
        handshakeLoop.exec();

    check(!failed && passiveConnected && activeConnected,
          "queued pre-start DTLS ClientHello did not complete the handshake");
}

static void runPair(const QCA::Certificate &cert, const QCA::PrivateKey &key, bool wrongFingerprint, bool requireSRTP,
                    bool peerSRTP)
{
    Dtls          offerer;
    auto          answererOwner = std::make_unique<Dtls>();
    auto         &answerer      = *answererOwner;
    const QString profile       = QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80");
    const QStringList profiles { profile };

    offerer.setNegotiationDeferred(true);
    answerer.setNegotiationDeferred(true);
    check(offerer.setSRTPProfiles(requireSRTP ? profiles : QStringList()), "offer profiles rejected");
    check(answerer.setSRTPProfiles(peerSRTP ? profiles : QStringList()), "answer profiles rejected");
    check(!offerer.setSRTPProfiles({ QStringLiteral("not-an-srtp-profile") }), "unsupported profile accepted");

    offerer.setLocalCertificate(cert, key);
    answerer.setLocalCertificate(cert, key);
    check(offerer.localCertificate().toDER() == cert.toDER(), "local identity not retained");
    check(offerer.srtpKeyingMaterial().isNull(), "keys exposed before handshake");

    SecureRtpAssociation offerAssociation(&offerer, QByteArrayLiteral("offer"));
    SecureRtpAssociation answerAssociation(&answerer, QByteArrayLiteral("answer"));
    check(!offerAssociation.isReady() && !answerAssociation.isReady(),
          "secure RTP association activated before DTLS authentication");

    int offerActivations = 0, offerInvalidations = 0;
    QObject::connect(&offerAssociation, &SecureRtpAssociation::ready, &offerAssociation,
                     [&](quint64) { ++offerActivations; });
    QObject::connect(&offerAssociation, &SecureRtpAssociation::invalidated, &offerAssociation,
                     [&](quint64) { ++offerInvalidations; });

    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    bool firstConnected = false, secondConnected = false, failed = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto connected = [&]() {
        if (firstConnected && secondConnected)
            loop.quit();
    };
    QObject::connect(&offerer, &Dtls::connected, &loop, [&]() {
        firstConnected = true;
        connected();
    });
    QObject::connect(&answerer, &Dtls::connected, &loop, [&]() {
        secondConnected = true;
        connected();
    });
    auto error = [&]() {
        failed = true;
        loop.quit();
    };
    QObject::connect(&offerer, &Dtls::errorOccurred, &loop, error);
    QObject::connect(&answerer, &Dtls::errorOccurred, &loop, error);

    // Exercise the exact ICE mux boundary in both directions: every DTLS
    // handshake/application record must be classified as DTLS and forwarded to
    // QCA rather than being consumed as protected RTP.
    QObject::connect(
        &offerer, &Dtls::readyReadOutgoing, &answerAssociation,
        [&]() {
            for (auto packet = offerer.readOutgoingDatagram(); !packet.isEmpty();
                 packet      = offerer.readOutgoingDatagram())
                check(answerAssociation.receiveMuxed(packet), "offerer DTLS record rejected by secure RTP mux");
        },
        Qt::QueuedConnection);
    QObject::connect(
        &answerer, &Dtls::readyReadOutgoing, &offerAssociation,
        [&]() {
            for (auto packet = answerer.readOutgoingDatagram(); !packet.isEmpty();
                 packet      = answerer.readOutgoingDatagram())
                check(offerAssociation.receiveMuxed(packet), "answerer DTLS record rejected by secure RTP mux");
        },
        Qt::QueuedConnection);

    offerer.initOutgoing();
    answerer.setRemoteFingerprint(offerer.localFingerprint());
    answerer.acceptIncoming();
    auto fingerprint = answerer.localFingerprint();
    if (wrongFingerprint)
        fingerprint.hash = XMPP::Hash(XMPP::Hash::Sha256, QByteArray(32, '\0'));
    offerer.setRemoteFingerprint(fingerprint);
    check(!offerer.isStarted() && !answerer.isStarted(),
          "deferred DTLS started before transport readiness");
    offerer.onRemoteAcceptedFingerprint();
    answerer.onRemoteAcceptedFingerprint();
    check(offerer.isStarted() && answerer.isStarted(),
          "deferred DTLS did not start after transport readiness");
    check(!offerer.setSRTPProfiles({}), "profile configuration changed after start");

    timer.start(5000);
    if (!failed && !(firstConnected && secondConnected))
        loop.exec();

    if (wrongFingerprint || (requireSRTP && !peerSRTP)) {
        check(failed && !firstConnected, "unsafe negotiation accepted or timed out");
        check(offerer.srtpKeyingMaterial().isNull(), "keys exposed after failed authentication/negotiation");
        check(!offerAssociation.isReady() && offerActivations == 0,
              "failed authentication activated secure RTP association");
        return;
    }

    check(!failed && firstConnected && secondConnected, "DTLS pair failed or timed out");
    const auto first  = offerer.srtpKeyingMaterial();
    const auto second = answerer.srtpKeyingMaterial();

    if (!requireSRTP) {
        check(first.isNull() && second.isNull(), "plain DTLS unexpectedly exported SRTP keys");
        check(!offerAssociation.isReady() && offerActivations == 0,
              "plain DTLS activated secure RTP association");

        QByteArray received;
        const auto reader = QObject::connect(&answerer, &Dtls::readyRead, &loop, [&]() {
            received = answerer.readDatagram();
            loop.quit();
        });
        timer.start(5000);
        offerer.writeDatagram(QByteArrayLiteral("data-channel-payload"));
        loop.exec();
        check(received == QByteArrayLiteral("data-channel-payload"),
              "plain DTLS application data did not cross secure RTP mux");
        QObject::disconnect(reader);
#ifdef IRIS_TEST_SCTP
        runDataChannel(offerer, answerer);
#endif
        return;
    }

    check(first.profile() == profile && second.profile() == profile, "wrong SRTP profile");
    check(first.localMasterKey().size() == 16 && first.localMasterSalt().size() == 14,
          "incorrect AES128-CM key/salt lengths");
    check(first.localMasterKey() == second.remoteMasterKey() && first.remoteMasterKey() == second.localMasterKey()
              && first.localMasterSalt() == second.remoteMasterSalt()
              && first.remoteMasterSalt() == second.localMasterSalt(),
          "directional SRTP keys do not match");

    check(offerAssociation.isReady() && answerAssociation.isReady() && offerActivations == 1,
          "verified DTLS did not activate secure RTP associations exactly once");
    const auto offerEpoch = offerAssociation.epoch();
    const auto &exported  = offerAssociation.keyingMaterial();
    check(exported.profile == first.profile() && exported.localMasterKey == first.localMasterKey()
              && exported.localMasterSalt == first.localMasterSalt()
              && exported.remoteMasterKey == first.remoteMasterKey()
              && exported.remoteMasterSalt == first.remoteMasterSalt(),
          "secure RTP association did not export verified DTLS key material");

    int protectedPackets = 0;
    QByteArray protectedData;
    PacketKind protectedKind = PacketKind::Rtp;
    QObject::connect(&answerAssociation, &SecureRtpAssociation::protectedPacketReceived, &answerAssociation,
                     [&](const QByteArray &data, PacketKind kind, quint64 epoch) {
                         check(epoch == answerAssociation.epoch(), "protected packet carried stale epoch");
                         protectedData = data;
                         protectedKind = kind;
                         ++protectedPackets;
                     });

    const auto rtp = QByteArray::fromHex("806000010000000100000002") + QByteArrayLiteral("opaque-srtp");
    check(answerAssociation.receiveMuxed(rtp), "protected RTP was rejected by mux");
    check(protectedPackets == 1 && protectedData == rtp && protectedKind == PacketKind::Rtp,
          "protected RTP was not forwarded unchanged to media backend");
    check(offerAssociation.validateProtectedMuxed(rtp, PacketKind::Rtp, offerEpoch),
          "valid protected RTP egress rejected");
    check(!offerAssociation.validateProtectedMuxed(rtp, PacketKind::Rtp, offerEpoch - 1),
          "stale protected RTP epoch accepted");

    auto ambiguous = rtp;
    ambiguous[1] = char(72);
    check(!offerAssociation.validateProtectedMuxed(ambiguous, PacketKind::Rtp, offerEpoch),
          "RTP/RTCP ambiguous payload type accepted");

    const auto rtcp = QByteArray::fromHex("80c9000100000002") + QByteArrayLiteral("opaque-srtcp");
    check(answerAssociation.receiveMuxed(rtcp), "protected RTCP was rejected by mux");
    check(protectedPackets == 2 && protectedData == rtcp && protectedKind == PacketKind::Rtcp,
          "protected RTCP was not forwarded unchanged to media backend");
    check(!answerAssociation.receiveMuxed(QByteArray::fromHex("00000000")),
          "STUN-like packet was delivered as protected media");

#ifdef IRIS_TEST_SCTP
    // Crucial regression: SRTP is active on this exact DTLS association while
    // datachannel traffic still traverses SecureRtpAssociation -> Dtls -> SCTP.
    runDataChannel(offerer, answerer);
#endif

    fingerprint.hash = XMPP::Hash(XMPP::Hash::Sha256, QByteArray(32, '\1'));
    offerer.setRemoteFingerprint(fingerprint);
    check(offerer.srtpKeyingMaterial().isNull(), "keys retained across fingerprint change");
    check(offerer.selectedSRTPProfile().isEmpty(), "profile exposed after fingerprint change");
    check(!offerAssociation.isReady() && offerAssociation.epoch() != offerEpoch && offerInvalidations == 1,
          "fingerprint change did not invalidate secure RTP association");
    check(!offerAssociation.validateProtectedMuxed(rtp, PacketKind::Rtp, offerAssociation.epoch()),
          "protected media accepted after DTLS identity invalidation");

    offerer.writeDatagram(QByteArrayLiteral("must-not-be-sent"));
    QTimer::singleShot(50, &loop, &QEventLoop::quit);
    loop.exec();
    for (auto data = answerer.readDatagram(); !data.isEmpty(); data = answerer.readDatagram())
        check(data != QByteArrayLiteral("must-not-be-sent"),
              "application data sent with an invalidated peer identity");

    SecureRtpAssociation detached(&answerer, QByteArrayLiteral("detached"));
    check(detached.isReady(), "late secure RTP association attachment failed");
    const auto detachedEpoch = detached.epoch();
    detached.close();
    check(!detached.isReady() && detached.epoch() != detachedEpoch,
          "closed secure RTP association remained active");
    const auto closedEpoch = detached.epoch();
    detached.close();
    check(detached.epoch() == closedEpoch, "secure RTP association close is not idempotent");

    const auto answerEpoch = answerAssociation.epoch();
    answererOwner.reset();
    check(!answerAssociation.isReady() && answerAssociation.epoch() != answerEpoch,
          "DTLS destruction left secure RTP association usable");
}
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer init;
#if QCA_MAJOR_VERSION >= 3
    check(Dtls::supportedSRTPProfiles().contains(QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80")),
          "a QCA3 DTLS-SRTP provider is required for this test");

    QCA::CertificateOptions options;
    QCA::CertificateInfo    info;
    info.insert(QCA::CommonName, QStringLiteral("iris-dtls-test"));
    options.setInfo(info);
    options.setSerialNumber(QCA::BigInteger(1));
    options.setValidityPeriod(QDateTime::currentDateTimeUtc().addDays(-1),
                              QDateTime::currentDateTimeUtc().addDays(1));
    const auto             key = QCA::KeyGenerator().createRSA(2048);
    const QCA::Certificate cert(options, key);
    check(!cert.isNull(), "certificate generation failed");

    runDeferredFirstFlight(cert, key);
    runPair(cert, key, false, true, true);
    runPair(cert, key, true, true, true);
    runPair(cert, key, false, true, false);
    runPair(cert, key, false, false, false);
    qInfo("Iris/QCA3 DTLS-SRTP key-export and SCTP mux integration passed");
#else
    Dtls dtls;
    check(!dtls.setSRTPProfiles({ QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80") }), "QCA2 accepted SRTP");
    check(dtls.setSRTPProfiles({}), "plain DTLS rejected");
#endif
}
