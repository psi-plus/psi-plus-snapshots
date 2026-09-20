// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QtEndian>
#include <cstring>
#include <iris/jingle-rtp-srtp.h>
using XMPP::Jingle::RTP::SrtpContext;
using Packet = SrtpContext::Packet;
static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
static QCA::SecureArray secret(int size, char value)
{
    QCA::SecureArray key(size);
    std::memset(key.data(), value, size_t(size));
    return key;
}
static QByteArray rtp(quint16 sequence, quint32 ssrc = 0x11223344)
{
    auto p = QByteArray::fromHex("806f000000000001000000007061796c6f6164");
    qToBigEndian(sequence, reinterpret_cast<uchar *>(p.data()) + 2);
    qToBigEndian(ssrc, reinterpret_cast<uchar *>(p.data()) + 8);
    return p;
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    using namespace XMPP::Jingle::RTP;
    check(classifyDatagram({}) == DatagramKind::Unknown, "empty datagram classified");
    for (int first = 0; first < 256; ++first) {
        QByteArray bytes(2, char(96));
        bytes[0]      = char(first);
        auto expected = DatagramKind::Unknown;
        if (first <= 3)
            expected = DatagramKind::Stun;
        else if (first >= 16 && first <= 19)
            expected = DatagramKind::Zrtp;
        else if (first >= 20 && first <= 63)
            expected = DatagramKind::Dtls;
        else if (first >= 64 && first <= 79)
            expected = DatagramKind::Turn;
        else if (first >= 128 && first <= 191)
            expected = DatagramKind::Rtp;
        check(classifyDatagram(bytes) == expected, "incorrect RFC 7983 boundary");
    }
    for (int second = 0; second < 256; ++second) {
        QByteArray bytes(2, char(second));
        bytes[0] = char(128);
        check(classifyDatagram(bytes) == (second >= 192 && second <= 223 ? DatagramKind::Rtcp : DatagramKind::Rtp),
              "incorrect RTP/RTCP multiplexing boundary");
    }
    check(classifyDatagram(QByteArray(1, char(128))) == DatagramKind::Unknown, "truncated RTP header classified");
    SrtpContext empty;
    check(!empty.protect(rtp(1), Packet::Rtp), "unconfigured SRTP accepted media");
    const auto profiles = SrtpContext::supportedProfiles();
#ifdef IRIS_TEST_SRTP
    check(profiles.contains("SRTP_AES128_CM_HMAC_SHA1_80"), "libSRTP baseline profile unavailable");
#else
    check(profiles.isEmpty(), "disabled SRTP reported available profiles");
#endif
    for (const auto &profile : profiles) {
        const bool  gcm      = profile.contains("GCM");
        const int   keySize  = profile.contains("256") ? 32 : 16;
        const int   saltSize = gcm ? 12 : 14;
        auto        a = secret(keySize, 'a'), b = secret(keySize, 'b');
        auto        saltA = secret(saltSize, 'c'), saltB = secret(saltSize, 'd');
        SrtpContext sender, receiver;
        check(sender.configure(profile, a, saltA, b, saltB) && receiver.configure(profile, b, saltB, a, saltA),
              "SRTP profile setup failed");
        auto cipher = sender.protect(rtp(1), Packet::Rtp);
        check(cipher && cipher->size() > rtp(1).size() && *cipher != rtp(1), "RTP not protected");
        auto plain = receiver.unprotect(*cipher, Packet::Rtp);
        check(plain && *plain == rtp(1), "SRTP roundtrip failed");
        check(!receiver.unprotect(*cipher, Packet::Rtp) && receiver.lastError() == SrtpContext::Error::Replay,
              "RTP replay accepted");
        check(receiver.configure(profile, b, saltB, a, saltA), "idempotent key setup failed");
        check(!receiver.unprotect(*cipher, Packet::Rtp), "identical key setup reset replay protection");
        check(!sender.protect(rtp(1), Packet::Rtp), "repeated sender sequence accepted");
        auto next = sender.protect(rtp(2), Packet::Rtp);
        check(bool(next), "next RTP packet rejected");
        auto altered                = *next;
        altered[altered.size() - 1] = char(uchar(altered.back()) ^ 1);
        check(!receiver.unprotect(altered, Packet::Rtp), "tampered RTP authenticated");
        check(receiver.unprotect(*next, Packet::Rtp).has_value(), "tampering consumed valid replay index");
        check(!sender.unprotect(*next, Packet::Rtp), "wrong directional key authenticated RTP");
        auto reply = receiver.protect(rtp(1, 0x55667788), Packet::Rtp);
        check(reply && sender.unprotect(*reply, Packet::Rtp) == std::optional<QByteArray>(rtp(1, 0x55667788)),
              "reverse SRTP direction failed");
        const auto rr            = QByteArray::fromHex("80c9000111223344");
        auto       protectedRtcp = sender.protect(rr, Packet::Rtcp);
        check(protectedRtcp && receiver.unprotect(*protectedRtcp, Packet::Rtcp) == std::optional<QByteArray>(rr),
              "SRTCP roundtrip failed");
        check(!receiver.unprotect(*protectedRtcp, Packet::Rtcp), "SRTCP replay accepted");
        check(!sender.protect({}, Packet::Rtp) && !sender.protect(QByteArray(70000, 'x'), Packet::Rtp),
              "invalid packet length accepted");

        SrtpContext rolloverSender, rolloverReceiver;
        check(rolloverSender.configure(profile, a, saltA, b, saltB)
                  && rolloverReceiver.configure(profile, b, saltB, a, saltA),
              "rollover setup failed");
        for (quint16 seq : { quint16(65534), quint16(65535), quint16(0), quint16(1) }) {
            auto c = rolloverSender.protect(rtp(seq), Packet::Rtp);
            check(c && rolloverReceiver.unprotect(*c, Packet::Rtp) == std::optional<QByteArray>(rtp(seq)),
                  "RTP rollover failed");
        }
        SrtpContext bounded;
        check(bounded.configure(profile, a, saltA, b, saltB), "SSRC-limit setup failed");
        for (quint32 ssrc = 1; ssrc <= 64; ++ssrc)
            check(bounded.protect(rtp(1, ssrc), Packet::Rtp).has_value(), "valid SSRC rejected");
        check(!bounded.protect(rtp(1, 65), Packet::Rtp) && bounded.lastError() == SrtpContext::Error::StreamLimit,
              "unbounded SRTP stream allocation");
        check(!sender.configure(profile, {}, saltA, b, saltB) && !sender.isReady(),
              "invalid rekey retained an old context");
        receiver.reset();
        check(!receiver.unprotect(*next, Packet::Rtp), "reset retained media keys");
        check(!receiver.configure("SRTP_NULL_HMAC_SHA1_80", a, saltA, b, saltB), "unencrypted SRTP policy accepted");
        qInfo("SRTP/SRTCP passed: %s", qPrintable(profile));
    }
}
