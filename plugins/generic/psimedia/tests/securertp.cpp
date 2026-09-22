/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "securertpcontext.h"

#include <QByteArray>
#include <QDebug>
#include <QStringList>
#include <QtEndian>

#include <cstdlib>

using namespace PsiMedia;

namespace {

[[noreturn]] void fail(const char *message)
{
    qCritical().noquote() << message;
    std::exit(1);
}

void require(bool condition, const char *message)
{
    if (!condition)
        fail(message);
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
    fail("unexpected SRTP profile");
}

QByteArray rtpPacket(quint16 sequence, quint32 ssrc)
{
    QByteArray packet(20, char(0));
    packet[0] = char(0x80);
    packet[1] = char(111);
    qToBigEndian<quint16>(sequence, reinterpret_cast<uchar *>(packet.data()) + 2);
    qToBigEndian<quint32>(0x01020304u + sequence, reinterpret_cast<uchar *>(packet.data()) + 4);
    qToBigEndian<quint32>(ssrc, reinterpret_cast<uchar *>(packet.data()) + 8);
    for (int i = 12; i < packet.size(); ++i)
        packet[i] = char(i);
    return packet;
}

QByteArray rtcpPacket(quint32 ssrc)
{
    // Minimal valid receiver report: V=2, RC=0, PT=201, length=1.
    QByteArray packet(8, char(0));
    packet[0] = char(0x80);
    packet[1] = char(201);
    packet[2] = char(0);
    packet[3] = char(1);
    qToBigEndian<quint32>(ssrc, reinterpret_cast<uchar *>(packet.data()) + 4);
    return packet;
}

PSecureRtpPacket packet(const QByteArray &id, quint64 epoch, PRtpPacket::Type type, QByteArray data)
{
    PSecureRtpPacket result;
    result.associationId = id;
    result.epoch         = epoch;
    result.type          = type;
    result.rawValue      = std::move(data);
    return result;
}

struct Pair {
    QByteArray      id = QByteArrayLiteral("test-association");
    QByteArray      aKey;
    QByteArray      aSalt;
    QByteArray      bKey;
    QByteArray      bSalt;
    SrtpAssociation a;
    SrtpAssociation b;

    Pair(const QString &profile, quint64 epoch = 1)
    {
        const auto sizes = sizesFor(profile);
        aKey             = QByteArray(sizes.key, char(0x11));
        aSalt            = QByteArray(sizes.salt, char(0x22));
        bKey             = QByteArray(sizes.key, char(0x33));
        bSalt            = QByteArray(sizes.salt, char(0x44));

        require(a.configure(id, epoch, profile, aKey, aSalt, bKey, bSalt), "sender configure failed");
        require(b.configure(id, epoch, profile, bKey, bSalt, aKey, aSalt), "receiver configure failed");
    }
};

void testProfileRoundTrips(const QString &profile)
{
    Pair pair(profile);

    const auto       rtp = packet(pair.id, 1, PRtpPacket::Type::Rtp, rtpPacket(1, 0x11223344));
    PSecureRtpPacket protectedRtp;
    PSecureRtpPacket plainRtp;
    require(pair.a.protect(rtp, &protectedRtp), "RTP protect failed");
    require(protectedRtp.rawValue != rtp.rawValue, "RTP protection did not change packet");
    require(pair.b.unprotect(protectedRtp, &plainRtp), "RTP unprotect failed");
    require(plainRtp.rawValue == rtp.rawValue, "RTP round trip mismatch");
    require(plainRtp.associationId == pair.id && plainRtp.epoch == 1, "RTP metadata mismatch");

    const auto       rtcp = packet(pair.id, 1, PRtpPacket::Type::Rtcp, rtcpPacket(0x11223344));
    PSecureRtpPacket protectedRtcp;
    PSecureRtpPacket plainRtcp;
    require(pair.a.protect(rtcp, &protectedRtcp), "RTCP protect failed");
    require(pair.b.unprotect(protectedRtcp, &plainRtcp), "RTCP unprotect failed");
    require(plainRtcp.rawValue == rtcp.rawValue, "RTCP round trip mismatch");
}

void testReplayAndEpoch(const QString &profile)
{
    Pair       pair(profile);
    const auto original = packet(pair.id, 1, PRtpPacket::Type::Rtp, rtpPacket(7, 0x55667788));

    PSecureRtpPacket encrypted;
    PSecureRtpPacket plain;
    require(pair.a.protect(original, &encrypted), "initial protect failed");
    require(pair.b.unprotect(encrypted, &plain), "initial unprotect failed");
    require(!pair.b.unprotect(encrypted, &plain), "replay was accepted");
    require(pair.b.lastError() == SecureRtpSessionContext::Error::Replay, "replay error not reported");

    // The same association and keys may advance the external security epoch,
    // but replay/ROC/SRTCP state must survive.
    require(pair.a.configure(pair.id, 2, profile, pair.aKey, pair.aSalt, pair.bKey, pair.bSalt),
            "sender identical-key reactivation failed");
    require(pair.b.configure(pair.id, 2, profile, pair.bKey, pair.bSalt, pair.aKey, pair.aSalt),
            "receiver identical-key reactivation failed");
    encrypted.epoch = 2;
    require(!pair.b.unprotect(encrypted, &plain), "replay window reset across identical-key reactivation");
    require(pair.b.lastError() == SecureRtpSessionContext::Error::Replay, "reactivated replay error not reported");

    pair.b.invalidate(pair.id, 1);
    require(pair.b.isReady() && pair.b.epoch() == 2, "stale invalidation killed current association");
    pair.b.invalidate(pair.id, 2);
    require(!pair.b.isReady(), "matching invalidation did not reset association");
}

void testTamper(const QString &profile)
{
    Pair       pair(profile);
    const auto original = packet(pair.id, 1, PRtpPacket::Type::Rtp, rtpPacket(19, 0x10203040));

    PSecureRtpPacket encrypted;
    PSecureRtpPacket plain;
    require(pair.a.protect(original, &encrypted), "tamper test protect failed");
    const int last           = encrypted.rawValue.size() - 1;
    encrypted.rawValue[last] = char(quint8(encrypted.rawValue[last]) ^ 0x01);
    require(!pair.b.unprotect(encrypted, &plain), "tampered packet was accepted");
    require(pair.b.lastError() == SecureRtpSessionContext::Error::Authentication,
            "tamper did not report authentication failure");
}

void testStalePacketAndInvalidReconfigure(const QString &profile)
{
    Pair             pair(profile, 3);
    auto             stale = packet(pair.id, 2, PRtpPacket::Type::Rtp, rtpPacket(2, 0x0a0b0c0d));
    PSecureRtpPacket output;
    require(!pair.a.protect(stale, &output), "stale epoch packet was accepted");
    require(pair.a.lastError() == SecureRtpSessionContext::Error::StaleEpoch, "stale epoch error not reported");
    require(pair.a.isReady(), "stale packet invalidated current association");

    QByteArray conflictingKey = pair.aKey;
    conflictingKey[0]         = char(quint8(conflictingKey[0]) ^ 0x01);
    require(!pair.a.configure(pair.id, 3, profile, conflictingKey, pair.aSalt, pair.bKey, pair.bSalt),
            "same-epoch key replacement succeeded");
    require(pair.a.lastError() == SecureRtpSessionContext::Error::StaleEpoch,
            "same-epoch key replacement did not report epoch conflict");
    require(pair.a.isReady(), "same-epoch key conflict destroyed the active association");

    QByteArray badKey(1, char(0));
    require(!pair.a.configure(pair.id, 4, profile, badKey, pair.aSalt, pair.bKey, pair.bSalt),
            "invalid key reconfiguration succeeded");
    require(!pair.a.isReady(), "invalid key reconfiguration retained old crypto state");
    require(pair.a.lastError() == SecureRtpSessionContext::Error::InvalidKey,
            "invalid key reconfiguration error mismatch");
}

void testStreamLimit(const QString &profile)
{
    Pair             pair(profile);
    PSecureRtpPacket output;
    for (quint32 i = 0; i < 64; ++i) {
        const auto p = packet(pair.id, 1, PRtpPacket::Type::Rtp, rtpPacket(1, 0x10000000u + i));
        require(pair.a.protect(p, &output), "packet before SSRC limit failed");
    }
    const auto overflow = packet(pair.id, 1, PRtpPacket::Type::Rtp, rtpPacket(1, 0x20000000u));
    require(!pair.a.protect(overflow, &output), "65th SSRC was accepted");
    require(pair.a.lastError() == SecureRtpSessionContext::Error::StreamLimit, "SSRC limit error mismatch");
}

} // namespace

int main()
{
    const auto profiles = SrtpAssociation::supportedProfiles();
    require(!profiles.isEmpty(), "libSRTP reported no usable secure profiles");

    // Probe means usable: every advertised profile must create both RTP and RTCP
    // contexts and complete an authenticated round trip.
    for (const auto &profile : profiles)
        testProfileRoundTrips(profile);

    const QString primary = profiles.contains(QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80"))
        ? QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80")
        : profiles.constFirst();

    testReplayAndEpoch(primary);
    testTamper(primary);
    testStalePacketAndInvalidReconfigure(primary);
    testStreamLimit(primary);

    return 0;
}
