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
    check(received && echoed, "SCTP over QCA DTLS failed or timed out");
}
#endif

static void runPair(const QCA::Certificate &cert, const QCA::PrivateKey &key, bool wrongFingerprint, bool requireSRTP,
                    bool peerSRTP)
{
    Dtls              offerer;
    auto              answererOwner = std::make_unique<Dtls>();
    auto             &answerer      = *answererOwner;
    const QString     profile       = QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80");
    const QStringList profiles { profile };
    check(offerer.setSRTPProfiles(requireSRTP ? profiles : QStringList()), "offer profiles rejected");
    check(answerer.setSRTPProfiles(peerSRTP ? profiles : QStringList()), "answer profiles rejected");
    check(!offerer.setSRTPProfiles({ QStringLiteral("not-an-srtp-profile") }), "unsupported profile accepted");
    offerer.setLocalCertificate(cert, key);
    answerer.setLocalCertificate(cert, key);
    check(offerer.localCertificate().toDER() == cert.toDER(), "local identity not retained");
    check(offerer.srtpKeyingMaterial().isNull(), "keys exposed before handshake");
#ifdef IRIS_TEST_SRTP
    using XMPP::Jingle::RTP::SrtpContext;
    using XMPP::Jingle::RTP::SrtpSession;
    SrtpSession *observed = nullptr;
    QObject::connect(&offerer, &Dtls::needRestart, &offerer, [&]() {
        check(observed && !observed->isReady(), "earlier restart subscriber can still use SRTP");
    });
    SrtpSession protectedSender(&offerer);
    observed = &protectedSender;
    check(!protectedSender.isReady(), "SRTP activated before DTLS authentication");
    int activations = 0, invalidations = 0;
    QObject::connect(&protectedSender, &SrtpSession::ready, &protectedSender, [&]() { ++activations; });
    QObject::connect(&protectedSender, &SrtpSession::invalidated, &protectedSender, [&]() { ++invalidations; });
#endif

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
    QObject::connect(
        &offerer, &Dtls::readyReadOutgoing, &answerer,
        [&]() {
            for (auto packet = offerer.readOutgoingDatagram(); !packet.isEmpty();
                 packet      = offerer.readOutgoingDatagram())
                answerer.writeIncomingDatagram(packet);
        },
        Qt::QueuedConnection);
    QObject::connect(
        &answerer, &Dtls::readyReadOutgoing, &offerer,
        [&]() {
            for (auto packet = answerer.readOutgoingDatagram(); !packet.isEmpty();
                 packet      = answerer.readOutgoingDatagram())
#ifdef IRIS_TEST_SRTP
                protectedSender.dispatchMuxed(packet);
#else
                offerer.writeIncomingDatagram(packet);
#endif
        },
        Qt::QueuedConnection);

    offerer.initOutgoing();
    answerer.setRemoteFingerprint(offerer.localFingerprint());
    answerer.acceptIncoming();
    auto fingerprint = answerer.localFingerprint();
    if (wrongFingerprint)
        fingerprint.hash = XMPP::Hash(XMPP::Hash::Sha256, QByteArray(32, '\0'));
    offerer.setRemoteFingerprint(fingerprint);
    answerer.onRemoteAcceptedFingerprint();
    check(!offerer.setSRTPProfiles({}), "profile configuration changed after start");
    timer.start(5000);
    if (!failed && !(firstConnected && secondConnected))
        loop.exec();
    if (wrongFingerprint || (requireSRTP && !peerSRTP)) {
        check(failed && !firstConnected, "unsafe negotiation accepted or timed out");
        check(offerer.srtpKeyingMaterial().isNull(), "keys exposed after failed authentication/negotiation");
#ifdef IRIS_TEST_SRTP
        check(!protectedSender.isReady() && activations == 0, "failed authentication activated SRTP");
#endif
        return;
    }
    check(!failed && firstConnected && secondConnected, "DTLS pair failed or timed out");
    const auto first  = offerer.srtpKeyingMaterial();
    const auto second = answerer.srtpKeyingMaterial();
    if (!requireSRTP) {
        check(first.isNull() && second.isNull(), "plain DTLS unexpectedly exported SRTP keys");
#ifdef IRIS_TEST_SRTP
        check(!protectedSender.isReady() && activations == 0, "plain DTLS activated SRTP");
#endif
        QByteArray received;
        const auto reader = QObject::connect(&answerer, &Dtls::readyRead, &loop, [&]() {
            received = answerer.readDatagram();
            loop.quit();
        });
        timer.start(5000);
        offerer.writeDatagram(QByteArrayLiteral("data-channel-payload"));
        loop.exec();
        check(received == QByteArrayLiteral("data-channel-payload"), "plain DTLS data transfer failed");
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
#ifdef IRIS_TEST_SRTP
    check(protectedSender.isReady() && activations == 1, "verified DTLS did not activate SRTP exactly once");
    SrtpSession protectedReceiver(&answerer); // Late attachment must also work.
    check(protectedReceiver.isReady(), "late SRTP attachment failed");
    const auto oldEpoch = protectedSender.epoch();
    const auto boundRtp = QByteArray::fromHex("806000010000000100000002") + QByteArrayLiteral("bound-media");
    check(!protectedSender.protect(boundRtp, SrtpContext::Packet::Rtp, oldEpoch - 1), "stale queued packet accepted");
    for (int pt : { 64, 72, 95, 200 }) {
        auto ambiguous = boundRtp;
        ambiguous[1]   = char(pt);
        check(!protectedSender.protectMuxed(ambiguous, SrtpContext::Packet::Rtp, oldEpoch),
              "ambiguous outgoing RTP/RTCP packet accepted");
    }
    check(!protectedReceiver.receiveMuxed(boundRtp), "plaintext media accepted by secure mux");
    check(!protectedReceiver.receiveMuxed(QByteArray::fromHex("00000000")), "STUN delivered as media");
    const auto boundEncrypted = protectedSender.protectMuxed(boundRtp, SrtpContext::Packet::Rtp, oldEpoch);
    check(boundEncrypted.has_value(), "bound SRTP protection failed");
    check(!protectedReceiver.unprotect(*boundEncrypted, SrtpContext::Packet::Rtp, protectedReceiver.epoch() - 1),
          "stale incoming epoch accepted");
    const auto muxed = protectedReceiver.receiveMuxed(*boundEncrypted);
    check(muxed && muxed->data == boundRtp && muxed->kind == SrtpContext::Packet::Rtp
              && muxed->epoch == protectedReceiver.epoch(),
          "bound SRTP reception failed");
    check(!protectedReceiver.receiveMuxed(*boundEncrypted), "multiplexed SRTP replay accepted");
    const auto boundRtcp = QByteArray::fromHex("80c9000100000002");
    auto       muxedRtcp = protectedSender.protectMuxed(boundRtcp, SrtpContext::Packet::Rtcp, oldEpoch);
    check(muxedRtcp.has_value(), "mux SRTCP protection failed");
    const auto control = protectedReceiver.receiveMuxed(*muxedRtcp);
    check(control && control->data == boundRtcp && control->kind == SrtpContext::Packet::Rtcp,
          "SRTCP was not demultiplexed");
    SrtpContext sender, receiver;
    check(sender.configure(first.profile(), first.localMasterKey(), first.localMasterSalt(), first.remoteMasterKey(),
                           first.remoteMasterSalt()),
          "DTLS sender SRTP configuration failed");
    check(receiver.configure(second.profile(), second.localMasterKey(), second.localMasterSalt(),
                             second.remoteMasterKey(), second.remoteMasterSalt()),
          "DTLS receiver SRTP configuration failed");
    const auto rtp       = QByteArray::fromHex("806000010000000100000001") + QByteArrayLiteral("media");
    const auto encrypted = sender.protect(rtp, SrtpContext::Packet::Rtp);
    check(encrypted.has_value(), "protect with DTLS-exported keys failed");
    check(receiver.unprotect(*encrypted, SrtpContext::Packet::Rtp) == rtp, "DTLS-exported keys do not decrypt SRTP");
    const auto rtcp          = QByteArray::fromHex("80c9000100000001");
    const auto protectedRtcp = receiver.protect(rtcp, SrtpContext::Packet::Rtcp);
    check(protectedRtcp.has_value(), "protect SRTCP with DTLS-exported keys failed");
    check(sender.unprotect(*protectedRtcp, SrtpContext::Packet::Rtcp) == rtcp,
          "DTLS-exported keys do not decrypt SRTCP");
#ifdef IRIS_TEST_SCTP
    // Keep live libSRTP contexts while exchanging SCTP over QCA application
    // records, then verify that neither path disturbed the other's state.
    runDataChannel(offerer, answerer);
    check(!receiver.unprotect(*encrypted, SrtpContext::Packet::Rtp)
              && receiver.lastError() == SrtpContext::Error::Replay,
          "SCTP disturbed SRTP replay state");
#endif
#endif
    fingerprint.hash = XMPP::Hash(XMPP::Hash::Sha256, QByteArray(32, '\1'));
    offerer.setRemoteFingerprint(fingerprint);
    check(offerer.srtpKeyingMaterial().isNull(), "keys retained across fingerprint change");
    check(offerer.selectedSRTPProfile().isEmpty(), "profile exposed after fingerprint change");
#ifdef IRIS_TEST_SRTP
    check(!protectedSender.isReady() && protectedSender.epoch() != oldEpoch && invalidations == 1,
          "fingerprint change did not invalidate bound SRTP");
    check(!protectedSender.protect(boundRtp, SrtpContext::Packet::Rtp, protectedSender.epoch()),
          "SRTP write accepted after invalidation");
#endif
    offerer.writeDatagram(QByteArrayLiteral("must-not-be-sent"));
    QTimer::singleShot(50, &loop, &QEventLoop::quit);
    loop.exec();
    // SCTP shutdown records sent before invalidation can still be in flight.
    // Reject the new write, not authenticated records from the preceding epoch.
    for (auto data = answerer.readDatagram(); !data.isEmpty(); data = answerer.readDatagram())
        check(data != QByteArrayLiteral("must-not-be-sent"), "application data sent with an invalidated peer identity");
#ifdef IRIS_TEST_SRTP
    const auto  receiverEpoch = protectedReceiver.epoch();
    SrtpSession detached(&answerer);
    check(detached.isReady(), "detachment test has no verified context");
    detached.close();
    const auto closedEpoch = detached.epoch();
    check(!detached.isReady() && !detached.protectMuxed(boundRtp, SrtpContext::Packet::Rtp, closedEpoch),
          "closed binding still sends media");
    detached.close();
    check(detached.epoch() == closedEpoch, "binding close is not idempotent");
    answererOwner.reset();
    check(!protectedReceiver.isReady() && protectedReceiver.epoch() != receiverEpoch,
          "DTLS destruction left bound SRTP usable");
    check(!protectedReceiver.unprotect(*boundEncrypted, SrtpContext::Packet::Rtp, protectedReceiver.epoch()),
          "SRTP received after DTLS destruction");
#endif
}
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer init;
#ifdef IRIS_TEST_SRTP
    check(!XMPP::Jingle::RTP::SrtpContext::supportedProfiles().isEmpty(), "libSRTP backend unavailable");
#endif
#if QCA_MAJOR_VERSION >= 3
    check(!XMPP::Jingle::RTP::supportedSecureRtpProfiles().isEmpty(), "no common runtime DTLS-SRTP profile");
    check(Dtls::supportedSRTPProfiles().contains(QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80")),
          "a QCA3 DTLS-SRTP provider is required for this test");
    QCA::CertificateOptions options;
    QCA::CertificateInfo    info;
    info.insert(QCA::CommonName, QStringLiteral("iris-dtls-test"));
    options.setInfo(info);
    options.setSerialNumber(QCA::BigInteger(1));
    options.setValidityPeriod(QDateTime::currentDateTimeUtc().addDays(-1), QDateTime::currentDateTimeUtc().addDays(1));
    const auto             key = QCA::KeyGenerator().createRSA(2048);
    const QCA::Certificate cert(options, key);
    check(!cert.isNull(), "certificate generation failed");
    runPair(cert, key, false, true, true);
    runPair(cert, key, true, true, true);
    runPair(cert, key, false, true, false);
    runPair(cert, key, false, false, false);
    qInfo("Iris/QCA3 DTLS-SRTP integration passed");
#else
    Dtls dtls;
    check(!dtls.setSRTPProfiles({ QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80") }), "QCA2 accepted SRTP");
    check(dtls.setSRTPProfiles({}), "plain DTLS rejected");
#endif
}
