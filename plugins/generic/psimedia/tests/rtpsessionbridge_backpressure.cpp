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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include <chrono>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

constexpr quint32 LocalSsrc  = 0x10203040;
constexpr quint32 RemoteSsrc = 0x55667788;

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

quint16 read16(const QByteArray &packet)
{
    if (packet.size() < 4)
        return 0;
    const auto *p = reinterpret_cast<const uchar *>(packet.constData());
    return quint16(p[2] << 8) | quint16(p[3]);
}

QByteArray makeRtp(quint16 sequence, quint32 ssrc, int payloadBytes = 1)
{
    QByteArray packet(12 + payloadBytes, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x80;
    p[1]         = 111;
    put16(p + 2, sequence);
    put32(p + 4, quint32(sequence) * 960);
    put32(p + 8, ssrc);
    if (payloadBytes > 0)
        p[12] = 0x7f;
    return packet;
}

GstBuffer *bufferFor(const QByteArray &data, quint16 sequence)
{
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, gsize(data.size()), nullptr);
    if (!buffer)
        return nullptr;
    gst_buffer_fill(buffer, 0, data.constData(), gsize(data.size()));
    GST_BUFFER_PTS(buffer)      = quint64(sequence) * 20 * GST_MSECOND;
    GST_BUFFER_DTS(buffer)      = GST_BUFFER_PTS(buffer);
    GST_BUFFER_DURATION(buffer) = 20 * GST_MSECOND;
    return buffer;
}

bool sendOutgoing(PsiMedia::RtpSessionBridge &bridge, quint16 sequence, int payloadBytes = 1)
{
    GstBuffer *buffer = bufferFor(makeRtp(sequence, LocalSsrc, payloadBytes), sequence);
    if (!buffer)
        return false;
    const auto result = bridge.sendRtp(buffer);
    gst_buffer_unref(buffer);
    return result == GST_FLOW_OK;
}

void pumpFor(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(1ms);
    }
}

template<typename Predicate> bool waitWithoutOwnerEvents(Predicate predicate,
                                                          std::chrono::milliseconds timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

int runPacketLimits(const PsiMedia::PPayloadInfo &opus)
{
    std::vector<quint16> networkSequences;
    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 10;
    bridge.setNetworkPacketHandler([&](const auto &packet) {
        if (packet.type == PsiMedia::PRtpPacket::Type::Rtp)
            networkSequences.push_back(read16(packet.rawValue));
    });
    if (!bridge.start())
        return 11;

    constexpr quint16 packetCount = 600;
    for (quint16 sequence = 1; sequence <= packetCount; ++sequence) {
        if (!sendOutgoing(bridge, sequence))
            return 12;
    }

    // Do not process the Qt owner event loop: the GStreamer streaming thread may
    // enqueue packets, but the user handler cannot consume them. Reaching the
    // configured packet cap proves the slow-consumer path is exercised.
    if (!waitWithoutOwnerEvents([&] {
            return bridge.deliveryQueueStats().networkPackets == bridge.maxQueuedNetworkPackets();
        })) {
        qCritical() << "network queue never reached its packet cap";
        return 13;
    }
    // Give the streaming task time to process the rest while delivery remains
    // blocked. The queue must never grow past either public bound.
    std::this_thread::sleep_for(100ms);
    auto stats = bridge.deliveryQueueStats();
    if (stats.networkPackets > bridge.maxQueuedNetworkPackets()
        || stats.networkBytes > bridge.maxQueuedNetworkBytes()) {
        qCritical() << "network queue exceeded its bound" << stats.networkPackets << stats.networkBytes;
        return 14;
    }

    pumpFor(100ms);
    if (networkSequences.empty() || networkSequences.size() > size_t(bridge.maxQueuedNetworkPackets())) {
        qCritical() << "bounded network tail was not delivered" << networkSequences.size();
        return 15;
    }
    if (networkSequences.back() != packetCount) {
        qCritical() << "bounded network queue did not retain the newest RTP packet" << networkSequences.back();
        return 16;
    }
    if (networkSequences.front() == 1) {
        qCritical() << "slow-consumer overflow did not discard old RTP";
        return 17;
    }

    bridge.stop();
    return 0;
}

int runByteLimit(const PsiMedia::PPayloadInfo &opus)
{
    int deliveries = 0;
    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 20;
    bridge.setNetworkPacketHandler([&](const auto &packet) {
        if (packet.type == PsiMedia::PRtpPacket::Type::Rtp)
            ++deliveries;
    });
    if (!bridge.start())
        return 21;

    // One RTP buffer larger than the complete delivery byte budget must never
    // be retained for a stalled consumer.
    const int oversizedPayload = int(bridge.maxQueuedNetworkBytes()) + 1;
    if (!sendOutgoing(bridge, 700, oversizedPayload))
        return 22;
    if (!waitWithoutOwnerEvents([&] { return bridge.deliveryQueueStats().networkByteDrops > 0; })) {
        qCritical() << "oversized RTP did not exercise the delivery byte cap";
        return 23;
    }
    const auto stats = bridge.deliveryQueueStats();
    if (stats.networkPackets != 0 || stats.networkBytes != 0) {
        qCritical() << "oversized RTP remained queued" << stats.networkPackets << stats.networkBytes;
        return 24;
    }
    pumpFor(30ms);
    if (deliveries != 0)
        return 25;

    bridge.stop();
    return 0;
}

int runAgeLimit(const PsiMedia::PPayloadInfo &opus)
{
    int deliveries = 0;
    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 30;
    bridge.setNetworkPacketHandler([&](const auto &packet) {
        if (packet.type == PsiMedia::PRtpPacket::Type::Rtp)
            ++deliveries;
    });
    if (!bridge.start())
        return 31;
    if (!sendOutgoing(bridge, 800))
        return 32;
    if (!waitWithoutOwnerEvents([&] { return bridge.deliveryQueueStats().networkPackets == 1; }))
        return 33;

    std::this_thread::sleep_for(std::chrono::milliseconds(bridge.maxQueuedPacketAgeMs() + 100));
    pumpFor(30ms);
    const auto stats = bridge.deliveryQueueStats();
    if (deliveries != 0 || stats.networkPackets != 0 || stats.networkExpiredDrops == 0) {
        qCritical() << "stale RTP survived the delivery age bound" << deliveries << stats.networkPackets
                    << stats.networkExpiredDrops;
        return 34;
    }

    bridge.stop();
    return 0;
}

int runOwnerFairness(const PsiMedia::PPayloadInfo &opus)
{
    int deliveries = 0;
    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 40;

    bridge.setNetworkPacketHandler([&](const auto &packet) {
        if (packet.type != PsiMedia::PRtpPacket::Type::Rtp)
            return;
        ++deliveries;
        std::this_thread::sleep_for(2ms);
    });
    if (!bridge.start())
        return 41;

    // Fill the queue without servicing Qt events. A single unbounded drain
    // would then spend hundreds of milliseconds in user callbacks before an
    // already-due owner timer gets a chance to run.
    for (quint16 sequence = 1500; sequence < 2100; ++sequence) {
        if (!sendOutgoing(bridge, sequence))
            return 42;
    }
    if (!waitWithoutOwnerEvents([&] {
            return bridge.deliveryQueueStats().networkPackets == bridge.maxQueuedNetworkPackets();
        })) {
        qCritical() << "fairness queue never reached its packet cap";
        return 43;
    }

    QElapsedTimer elapsed;
    elapsed.start();
    qint64 timerLatencyMs = -1;
    QTimer ownerTimer;
    ownerTimer.setSingleShot(true);
    QObject::connect(&ownerTimer, &QTimer::timeout, [&]() { timerLatencyMs = elapsed.elapsed(); });
    ownerTimer.start(20);

    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (timerLatencyMs < 0 && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(1ms);
    }

    if (timerLatencyMs < 0 || timerLatencyMs > 150) {
        qCritical() << "delivery drain starved owner timer" << timerLatencyMs << "ms after" << deliveries
                    << "callbacks";
        return 44;
    }

    bridge.stop();
    return 0;
}

int runMediaPacketLimit(const PsiMedia::PPayloadInfo &opus)
{
    int mediaDeliveries = 0;
    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 50;
    bridge.setMediaPacketHandler([&](GstBuffer *) { ++mediaDeliveries; });
    if (!bridge.start())
        return 51;

    for (quint16 sequence = 1000; sequence < 1400; ++sequence) {
        PsiMedia::PRtpPacket packet;
        packet.type     = PsiMedia::PRtpPacket::Type::Rtp;
        packet.rawValue = makeRtp(sequence, RemoteSsrc);
        if (bridge.receivePacket(packet) != GST_FLOW_OK)
            return 52;
    }

    if (!waitWithoutOwnerEvents([&] {
            return bridge.deliveryQueueStats().mediaPackets == bridge.maxQueuedMediaPackets();
        })) {
        qCritical() << "media queue never reached its packet cap";
        return 53;
    }
    std::this_thread::sleep_for(100ms);
    const auto stats = bridge.deliveryQueueStats();
    if (stats.mediaPackets > bridge.maxQueuedMediaPackets() || stats.mediaBytes > bridge.maxQueuedMediaBytes())
        return 54;

    bridge.stop();
    pumpFor(30ms);
    // stop() invalidates the queued generation before the pending delivery
    // event can run; no stale media callback may escape afterwards.
    if (mediaDeliveries != 0)
        return 55;
    return 0;
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

    if (const int result = runPacketLimits(opus))
        return result;
    if (const int result = runByteLimit(opus))
        return result;
    if (const int result = runAgeLimit(opus))
        return result;
    if (const int result = runOwnerFairness(opus))
        return result;
    if (const int result = runMediaPacketLimit(opus))
        return result;

    qInfo() << "RTP bridge backpressure regression passed";
    return 0;
}
