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

constexpr quint32 LocalSsrc        = 0x10203040;
constexpr quint32 RemoteSsrc       = 0x55667788;
constexpr quint32 RemoteReportSsrc = 0x99aabbcc;

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

QByteArray makeRtp(quint16 sequence, quint32 timestamp, quint32 ssrc)
{
    QByteArray packet(13, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x80;
    p[1]         = 111;
    put16(p + 2, sequence);
    put32(p + 4, timestamp);
    put32(p + 8, ssrc);
    p[12] = 0x7f;
    return packet;
}

QByteArray makeReceiverReport(quint32 senderSsrc, quint32 reportedSsrc)
{
    // RFC 3550 RR with one report block. Distinct non-zero values make it
    // possible to prove that rtpsession parsed the report rather than merely
    // observed an RTCP buffer on recv_rtcp_sink.
    QByteArray packet(32, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x81; // V=2, RC=1
    p[1]         = 201;  // RR
    put16(p + 2, 7);     // (32 / 4) - 1
    put32(p + 4, senderSsrc);
    put32(p + 8, reportedSsrc);
    p[12] = 7;           // fraction lost
    p[13] = 0;
    p[14] = 0;
    p[15] = 3;           // cumulative packets lost
    put32(p + 16, 1);    // extended highest sequence
    put32(p + 20, 123);  // interarrival jitter
    return packet;
}

GstBuffer *bufferFor(const QByteArray &data, GstClockTime pts)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, gsize(data.size()), nullptr);
    if (!buffer)
        return nullptr;
    gst_buffer_fill(buffer, 0, data.constData(), gsize(data.size()));
    GST_BUFFER_PTS(buffer)      = pts;
    GST_BUFFER_DTS(buffer)      = pts;
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    return buffer;
}

QByteArray bytesFromBuffer(GstBuffer *buffer)
{
    if (!buffer)
        return {};
    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return {};
    const QByteArray result(reinterpret_cast<const char *>(map.data), int(map.size));
    gst_buffer_unmap(buffer, &map);
    return result;
}

const GstStructure *findSourceStats(const GstStructure *sessionStats, quint32 ssrc)
{
    if (!sessionStats)
        return nullptr;
    const GValue *sourcesValue = gst_structure_get_value(sessionStats, "source-stats");
    if (!sourcesValue)
        return nullptr;
    auto *sources = static_cast<GValueArray *>(g_value_get_boxed(sourcesValue));
    if (!sources)
        return nullptr;

    for (guint i = 0; i < sources->n_values; ++i) {
        const GValue *value = &sources->values[i];
        if (!value)
            continue;
        const auto *source = static_cast<const GstStructure *>(g_value_get_boxed(value));
        guint       sourceSsrc = 0;
        if (source && gst_structure_get_uint(source, "ssrc", &sourceSsrc) && sourceSsrc == ssrc)
            return source;
    }
    return nullptr;
}

bool remoteRtpWasProcessed(PsiMedia::RtpSessionBridge &bridge)
{
    GstStructure *stats = bridge.sessionStats();
    if (!stats)
        return false;
    const GstStructure *source = findSourceStats(stats, RemoteSsrc);
    gboolean            validated = FALSE;
    gboolean            internal  = TRUE;
    gint                clockRate = -1;
    guint64             packets   = 0;
    const bool ok = source && gst_structure_get_boolean(source, "validated", &validated)
        && gst_structure_get_boolean(source, "internal", &internal)
        && gst_structure_get_int(source, "clock-rate", &clockRate)
        && gst_structure_get_uint64(source, "packets-received", &packets) && validated && !internal
        && clockRate == 48000 && packets > 0;
    gst_structure_free(stats);
    return ok;
}

bool receiverReportWasProcessed(PsiMedia::RtpSessionBridge &bridge)
{
    GstStructure *stats = bridge.sessionStats();
    if (!stats)
        return false;

    // GStreamer 1.24 stores an incoming report block on the RTPSource that
    // sent the RR. rb-ssrc identifies the local source that the block reports
    // on. Checking both SSRCs and the non-zero fields proves that recv_rtcp_sink
    // did more than merely observe an RTCP buffer.
    const GstStructure *source = findSourceStats(stats, RemoteReportSsrc);
    gboolean            internal     = TRUE;
    gboolean            haveRb       = FALSE;
    guint               reportedSsrc = 0;
    guint               fractionLost = 0;
    gint                packetsLost  = 0;
    guint               highestSeq   = 0;
    guint               jitter       = 0;
    const bool ok = source && gst_structure_get_boolean(source, "internal", &internal)
        && gst_structure_get_boolean(source, "have-rb", &haveRb)
        && gst_structure_get_uint(source, "rb-ssrc", &reportedSsrc)
        && gst_structure_get_uint(source, "rb-fractionlost", &fractionLost)
        && gst_structure_get_int(source, "rb-packetslost", &packetsLost)
        && gst_structure_get_uint(source, "rb-exthighestseq", &highestSeq)
        && gst_structure_get_uint(source, "rb-jitter", &jitter) && !internal && haveRb
        && reportedSsrc == LocalSsrc && fractionLost == 7 && packetsLost == 3 && highestSeq == 1 && jitter == 123;
    gst_structure_free(stats);
    return ok;
}

bool senderReport(const QByteArray &compound, quint32 expectedSsrc, quint32 *rtpTimestamp = nullptr)
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

        if (packet[1] == 200 && packetBytes >= 28 && read32(packet + 4) == expectedSsrc) {
            const quint32 packetCount = read32(packet + 20);
            const quint32 octetCount  = read32(packet + 24);
            if (!packetCount || !octetCount)
                return false;
            if (rtpTimestamp)
                *rtpTimestamp = read32(packet + 16);
            return true;
        }
        offset += packetBytes;
    }
    return false;
}

} // namespace

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
    std::vector<QByteArray>           mediaPackets;

    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus })) {
        qCritical() << "failed to construct/configure RTP session bridge";
        return 1;
    }
    bridge.setNetworkPacketHandler([&](const PsiMedia::PRtpPacket &packet) { networkPackets.push_back(packet); });
    bridge.setMediaPacketHandler([&](GstBuffer *buffer) { mediaPackets.push_back(bytesFromBuffer(buffer)); });
    bridge.setRtcpMinimumInterval(10 * GST_MSECOND);
    if (!bridge.start()) {
        qCritical() << "failed to start RTP session bridge";
        return 2;
    }

    const QByteArray outgoing = makeRtp(1, 960, LocalSsrc);
    GstBuffer       *out      = bufferFor(outgoing, 20 * GST_MSECOND);
    if (!out) {
        qCritical() << "failed to allocate outgoing RTP buffer";
        return 3;
    }
    const auto sendResult = bridge.sendRtp(out);
    gst_buffer_unref(out);
    if (sendResult != GST_FLOW_OK) {
        qCritical() << "failed to feed outgoing RTP";
        return 4;
    }
    if (!waitUntil([&] {
            return std::any_of(networkPackets.cbegin(), networkPackets.cend(), [&](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtp && packet.rawValue == outgoing;
            });
        })) {
        qCritical() << "outgoing RTP bytes/PT/SSRC were not preserved";
        return 5;
    }

    // Three consecutive packets cross rtpsession's default RFC 3550 probation.
    // Require exact media bytes afterwards rather than merely a callback count.
    for (quint16 sequence = 10; sequence < 13; ++sequence) {
        PsiMedia::PRtpPacket packet;
        packet.type     = PsiMedia::PRtpPacket::Type::Rtp;
        packet.rawValue = makeRtp(sequence, quint32(sequence) * 960, RemoteSsrc);
        if (bridge.receivePacket(packet) != GST_FLOW_OK) {
            qCritical() << "failed to feed incoming RTP" << sequence;
            return 6;
        }
    }
    if (!waitUntil([&] {
            const auto second = makeRtp(11, 11 * 960, RemoteSsrc);
            const auto third  = makeRtp(12, 12 * 960, RemoteSsrc);
            return std::find(mediaPackets.cbegin(), mediaPackets.cend(), second) != mediaPackets.cend()
                || std::find(mediaPackets.cbegin(), mediaPackets.cend(), third) != mediaPackets.cend();
        })) {
        qCritical() << "validated incoming RTP bytes did not reach media output";
        return 7;
    }
    if (!waitUntil([&] { return remoteRtpWasProcessed(bridge); })) {
        qCritical() << "remote RTP source was not validated/accounted at 48 kHz";
        return 8;
    }

    PsiMedia::PRtpPacket rr;
    rr.type     = PsiMedia::PRtpPacket::Type::Rtcp;
    rr.rawValue = makeReceiverReport(RemoteReportSsrc, LocalSsrc);
    if (bridge.receivePacket(rr) != GST_FLOW_OK) {
        qCritical() << "failed to feed incoming receiver report";
        return 9;
    }
    if (!waitUntil([&] { return bridge.receivedRtcpPackets() > 0; })) {
        qCritical() << "incoming RTCP did not reach rtpsession";
        return 10;
    }
    if (!waitUntil([&] { return receiverReportWasProcessed(bridge); })) {
        qCritical() << "incoming RR fields were not applied to the reporting RTP source statistics";
        return 11;
    }

    // The same rtpsession that saw the outgoing RTP must now emit an SR for
    // that exact local SSRC with non-zero packet/octet counters.
    if (!waitUntil([&] { return bridge.requestRtcp(100 * GST_MSECOND); })) {
        qCritical() << "RTCP scheduler never became ready";
        return 12;
    }
    if (!waitUntil([&] {
            return std::any_of(networkPackets.cbegin(), networkPackets.cend(), [](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtcp && senderReport(packet.rawValue, LocalSsrc);
            });
        })) {
        qCritical() << "generated RTCP did not contain a sender report for the RTP sender SSRC";
        return 13;
    }

    bridge.stop();

    // A sender pipeline can queue encoded RTP before handing it to the bridge.
    // Preserve that producer age when mapping RTP to RTCP SR time instead of
    // pretending the packet was produced at callback arrival.
    constexpr quint32 TimingSsrc         = 0x1234abcd;
    constexpr quint32 TimingRtpTimestamp = 48000;
    std::vector<PsiMedia::PRtpPacket> timingPackets;

    PsiMedia::RtpSessionBridge timingBridge(QStringLiteral("audio"));
    if (!timingBridge.isValid() || !timingBridge.setPayloads({ opus }, { opus })) {
        qCritical() << "failed to configure timing bridge";
        return 14;
    }
    timingBridge.setNetworkPacketHandler(
        [&](const PsiMedia::PRtpPacket &packet) { timingPackets.push_back(packet); });
    timingBridge.setRtcpMinimumInterval(10 * GST_MSECOND);
    if (!timingBridge.start()) {
        qCritical() << "failed to start timing bridge";
        return 15;
    }

    std::this_thread::sleep_for(600ms);
    const QByteArray timingRtp = makeRtp(77, TimingRtpTimestamp, TimingSsrc);
    GstBuffer *timingBuffer = bufferFor(timingRtp, 100 * GST_MSECOND);
    if (!timingBuffer) {
        qCritical() << "failed to allocate timing RTP buffer";
        return 16;
    }
    const auto timingSend = timingBridge.sendRtp(timingBuffer, 500 * GST_MSECOND);
    gst_buffer_unref(timingBuffer);
    if (timingSend != GST_FLOW_OK) {
        qCritical() << "failed to feed aged RTP buffer";
        return 17;
    }
    if (!waitUntil([&] {
            return std::any_of(timingPackets.cbegin(), timingPackets.cend(), [&](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtp && packet.rawValue == timingRtp;
            });
        })) {
        qCritical() << "aged RTP bytes were not preserved";
        return 18;
    }

    if (!waitUntil([&] { return timingBridge.requestRtcp(100 * GST_MSECOND); })) {
        qCritical() << "timing RTCP scheduler never became ready";
        return 19;
    }

    quint32 srRtpTimestamp = 0;
    if (!waitUntil([&] {
            return std::any_of(timingPackets.cbegin(), timingPackets.cend(), [&](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtcp
                    && senderReport(packet.rawValue, TimingSsrc, &srRtpTimestamp);
            });
        })) {
        qCritical() << "timing bridge did not emit a sender report";
        return 20;
    }

    const quint32 srAdvance = srRtpTimestamp - TimingRtpTimestamp;
    if (srAdvance < 12000 || srAdvance > 72000) {
        qCritical() << "sender report ignored producer RTP age" << srAdvance << srRtpTimestamp;
        return 21;
    }

    timingBridge.stop();
    qInfo() << "RTP/RTCP packet semantics and producer-age timing regressions passed";
    return 0;
}
