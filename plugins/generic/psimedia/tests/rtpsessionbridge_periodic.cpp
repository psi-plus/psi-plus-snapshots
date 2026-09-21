/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "rtpsessionbridge.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr quint32 LocalSsrc = 0x10203040;

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

quint16 read16(const uchar *p) { return quint16(p[0] << 8) | quint16(p[1]); }

quint32 read32(const uchar *p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

QByteArray makeRtp()
{
    QByteArray packet(13, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x80;
    p[1]         = 111;
    put16(p + 2, 1);
    put32(p + 4, 960);
    put32(p + 8, LocalSsrc);
    p[12] = 0x7f;
    return packet;
}

GstBuffer *bufferFor(const QByteArray &data)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, gsize(data.size()), nullptr);
    if (!buffer)
        return nullptr;
    gst_buffer_fill(buffer, 0, data.constData(), gsize(data.size()));
    GST_BUFFER_PTS(buffer)      = 20 * GST_MSECOND;
    GST_BUFFER_DTS(buffer)      = 20 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    return buffer;
}

bool hasSenderReport(const QByteArray &compound)
{
    const auto *bytes = reinterpret_cast<const uchar *>(compound.constData());
    int         offset = 0;
    while (offset + 8 <= compound.size()) {
        const uchar *packet = bytes + offset;
        if ((packet[0] >> 6) != 2)
            return false;
        const int packetBytes = (int(read16(packet + 2)) + 1) * 4;
        if (packetBytes < 8 || offset + packetBytes > compound.size())
            return false;
        if (packet[1] == 200 && packetBytes >= 28 && read32(packet + 4) == LocalSsrc
            && read32(packet + 20) > 0 && read32(packet + 24) > 0)
            return true;
        offset += packetBytes;
    }
    return false;
}

template<typename Predicate> bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s)
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
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    PsiMedia::PPayloadInfo opus;
    opus.id        = 111;
    opus.name      = QStringLiteral("OPUS");
    opus.clockrate = 48000;
    opus.channels  = 2;

    std::vector<PsiMedia::PRtpPacket> networkPackets;
    PsiMedia::RtpSessionBridge        bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus })) {
        qCritical() << "failed to construct/configure periodic-RTCP bridge";
        return 1;
    }
    bridge.setNetworkPacketHandler([&](const auto &packet) { networkPackets.push_back(packet); });
    // This property controls regular RTCP scheduling. Keep it short so the test
    // remains deterministic without forcing an early report via requestRtcp().
    bridge.setRtcpMinimumInterval(10 * GST_MSECOND);
    if (!bridge.start()) {
        qCritical() << "failed to start periodic-RTCP bridge";
        return 2;
    }

    const QByteArray outgoing = makeRtp();
    GstBuffer       *buffer   = bufferFor(outgoing);
    if (!buffer)
        return 3;
    const auto result = bridge.sendRtp(buffer);
    gst_buffer_unref(buffer);
    if (result != GST_FLOW_OK) {
        qCritical() << "failed to feed RTP before regular RTCP scheduling";
        return 4;
    }

    if (!waitUntil([&] {
            return std::any_of(networkPackets.cbegin(), networkPackets.cend(), [&](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtp && packet.rawValue == outgoing;
            });
        })) {
        qCritical() << "outgoing RTP did not establish sender state";
        return 5;
    }

    // Deliberately do not call requestRtcp(). A passing result therefore proves
    // rtpsession's regular scheduler produced an SR from the same sender state.
    if (!waitUntil([&] {
            return std::any_of(networkPackets.cbegin(), networkPackets.cend(), [](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtcp && hasSenderReport(packet.rawValue);
            });
        })) {
        qCritical() << "regular RTCP scheduler did not emit an SR for the active sender";
        return 6;
    }

    bridge.stop();
    qInfo() << "RTP/RTCP regular scheduling regression passed";
    return 0;
}
