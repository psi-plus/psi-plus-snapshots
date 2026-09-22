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
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

enum class EarlyExit {
    None,
    AfterStart,
    AfterOutgoingFeed,
    AfterOutgoingWait,
    AfterIncomingFeed,
    AfterIncomingWait,
    AfterRtcpFeed,
    AfterRtcpWait,
    AfterRtcpRequest,
    AfterRtcpOutputWait,
};

template <typename Predicate> bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s)
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

void pumpFor(std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(1ms);
    }
}

QByteArray makeRtp(quint16 sequence, quint32 timestamp, quint32 ssrc)
{
    QByteArray packet(13, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x80;
    p[1]         = 111;
    p[2]         = uchar(sequence >> 8);
    p[3]         = uchar(sequence);
    p[4]         = uchar(timestamp >> 24);
    p[5]         = uchar(timestamp >> 16);
    p[6]         = uchar(timestamp >> 8);
    p[7]         = uchar(timestamp);
    p[8]         = uchar(ssrc >> 24);
    p[9]         = uchar(ssrc >> 16);
    p[10]        = uchar(ssrc >> 8);
    p[11]        = uchar(ssrc);
    p[12]        = 0x7f;
    return packet;
}

QByteArray makeReceiverReport(quint32 ssrc)
{
    QByteArray packet(8, '\0');
    auto      *p = reinterpret_cast<uchar *>(packet.data());
    p[0]         = 0x80;
    p[1]         = 201;
    p[2]         = 0;
    p[3]         = 1;
    p[4]         = uchar(ssrc >> 24);
    p[5]         = uchar(ssrc >> 16);
    p[6]         = uchar(ssrc >> 8);
    p[7]         = uchar(ssrc);
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

bool isRtcp(const QByteArray &packet)
{
    if (packet.size() < 4)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(packet.constData());
    return (p[0] >> 6) == 2 && p[1] >= 192 && p[1] <= 223;
}

PsiMedia::PPayloadInfo withParameter(PsiMedia::PPayloadInfo payload, const QString &name, const QString &value)
{
    PsiMedia::PPayloadInfo::Parameter parameter;
    parameter.name  = name;
    parameter.value = value;
    payload.parameters.append(parameter);
    return payload;
}

bool feedOutgoing(PsiMedia::RtpSessionBridge &bridge, quint16 sequence)
{
    const QByteArray outgoing = makeRtp(sequence, quint32(sequence) * 960, 0x10203040);
    GstBuffer       *out      = bufferFor(outgoing, quint64(sequence) * 20 * GST_MSECOND);
    if (!out)
        return false;
    const auto result = bridge.sendRtp(out);
    gst_buffer_unref(out);
    return result == GST_FLOW_OK;
}

int runScenario(const PsiMedia::PPayloadInfo &opus, EarlyExit earlyExit)
{
    // Callback-owned state is declared before the bridge so every return path
    // destroys the callback source first. All user callbacks must execute on
    // this Qt owner thread, never on a GStreamer streaming thread.
    std::vector<PsiMedia::PRtpPacket> networkPackets;
    int                               receivedMediaPackets = 0;
    bool                              wrongCallbackThread  = false;
    QThread                          *ownerThread          = QThread::currentThread();

    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid()) {
        qCritical() << "failed to create rtpsession bridge";
        return 1;
    }

    const auto local  = withParameter(opus, QStringLiteral("minptime"), QStringLiteral("10"));
    auto       remote = withParameter(opus, QStringLiteral("useinbandfec"), QStringLiteral("1"));
    if (!bridge.setPayloads({ local }, { remote })) {
        qCritical() << "failed to configure direction-specific PT maps";
        return 2;
    }

    bridge.setNetworkPacketHandler([&](const PsiMedia::PRtpPacket &packet) {
        wrongCallbackThread |= QThread::currentThread() != ownerThread;
        networkPackets.push_back(packet);
    });
    bridge.setMediaPacketHandler([&](GstBuffer *buffer) {
        wrongCallbackThread |= QThread::currentThread() != ownerThread;
        if (buffer && gst_buffer_get_size(buffer) >= 12)
            ++receivedMediaPackets;
    });
    bridge.setRtcpMinimumInterval(10 * GST_MSECOND);
    if (!bridge.start()) {
        qCritical() << "failed to start rtpsession bridge";
        return 3;
    }

    // Direction-specific fmtp is allowed to change while the RTP session is
    // running. The PT cache must be cleared without re-entering payloadMutex_.
    remote.parameters.clear();
    remote = withParameter(remote, QStringLiteral("useinbandfec"), QStringLiteral("0"));
    if (!bridge.setPayloads({ local }, { remote })) {
        qCritical() << "failed to update direction-specific PT maps while running";
        return 12;
    }

    if (earlyExit == EarlyExit::AfterStart)
        return 0;

    if (!feedOutgoing(bridge, 1)) {
        qCritical() << "failed to feed outgoing RTP";
        return 4;
    }
    if (earlyExit == EarlyExit::AfterOutgoingFeed)
        return 0;

    if (!waitUntil([&] {
            return std::any_of(networkPackets.cbegin(), networkPackets.cend(),
                               [](const auto &packet) { return packet.type == PsiMedia::PRtpPacket::Type::Rtp; });
        })) {
        qCritical() << "outgoing RTP was not emitted";
        return 5;
    }
    if (earlyExit == EarlyExit::AfterOutgoingWait)
        return 0;

    // rtpsession applies RFC 3550 probation to a newly observed remote SSRC.
    for (quint16 sequence = 10; sequence < 13; ++sequence) {
        PsiMedia::PRtpPacket packet;
        packet.rawValue = makeRtp(sequence, quint32(sequence) * 960, 0x55667788);
        packet.type     = PsiMedia::PRtpPacket::Type::Rtp;
        if (bridge.receivePacket(packet) != GST_FLOW_OK) {
            qCritical() << "failed to feed incoming RTP";
            return 6;
        }
    }
    if (earlyExit == EarlyExit::AfterIncomingFeed)
        return 0;

    if (!waitUntil([&] { return receivedMediaPackets > 0; })) {
        qCritical() << "incoming RTP did not reach the media side";
        return 7;
    }
    if (earlyExit == EarlyExit::AfterIncomingWait)
        return 0;

    PsiMedia::PRtpPacket rr;
    rr.rawValue = makeReceiverReport(0x99aabbcc);
    rr.type     = PsiMedia::PRtpPacket::Type::Rtcp;
    if (bridge.receivePacket(rr) != GST_FLOW_OK) {
        qCritical() << "failed to feed incoming RTCP";
        return 8;
    }
    if (earlyExit == EarlyExit::AfterRtcpFeed)
        return 0;

    if (!waitUntil([&] { return bridge.receivedRtcpPackets() > 0; })) {
        qCritical() << "incoming RTCP was not observed by rtpsession";
        return 9;
    }
    if (earlyExit == EarlyExit::AfterRtcpWait)
        return 0;

    // send-rtcp-full legitimately returns false until the rtpsession task has
    // initialized next_rtcp_check_time. Keep pumping the owner event loop until
    // the scheduler is ready, then still require an actual RTCP packet below.
    if (!waitUntil([&] { return bridge.requestRtcp(100 * GST_MSECOND); })) {
        qCritical() << "rtpsession scheduler did not become ready for RTCP";
        return 10;
    }
    if (earlyExit == EarlyExit::AfterRtcpRequest)
        return 0;

    if (!waitUntil([&] {
            return std::any_of(networkPackets.cbegin(), networkPackets.cend(), [](const auto &packet) {
                return packet.type == PsiMedia::PRtpPacket::Type::Rtcp && isRtcp(packet.rawValue);
            });
        })) {
        qCritical() << "outgoing RTCP was not emitted";
        return 11;
    }
    if (earlyExit == EarlyExit::AfterRtcpOutputWait)
        return 0;

    if (wrongCallbackThread) {
        qCritical() << "RTP bridge invoked a user handler outside its owner thread";
        return 13;
    }

    bridge.stop();
    return 0;
}

int runStopFromCallback(const PsiMedia::PPayloadInfo &opus)
{
    int      rtpDeliveries       = 0;
    bool     stoppedFromCallback = false;
    bool     wrongCallbackThread = false;
    QThread *ownerThread         = QThread::currentThread();

    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 40;

    bridge.setNetworkPacketHandler([&](const PsiMedia::PRtpPacket &packet) {
        wrongCallbackThread |= QThread::currentThread() != ownerThread;
        if (packet.type != PsiMedia::PRtpPacket::Type::Rtp)
            return;
        ++rtpDeliveries;
        if (!stoppedFromCallback) {
            stoppedFromCallback = true;
            bridge.stop();
        }
    });
    if (!bridge.start())
        return 41;

    // Queue several packets before pumping the owner event loop. The first
    // delivery stops the bridge; generation invalidation must discard the rest.
    for (quint16 sequence = 1; sequence <= 4; ++sequence) {
        if (!feedOutgoing(bridge, sequence))
            return 42;
    }
    if (!waitUntil([&] { return stoppedFromCallback; }))
        return 43;
    pumpFor(50ms);
    if (wrongCallbackThread || rtpDeliveries != 1)
        return 44;

    GstBuffer *afterStop = bufferFor(makeRtp(20, 19200, 0x10203040), 400 * GST_MSECOND);
    if (!afterStop)
        return 45;
    const auto stoppedResult = bridge.sendRtp(afterStop);
    gst_buffer_unref(afterStop);
    if (stoppedResult != GST_FLOW_FLUSHING)
        return 46;

    // A new start gets a new generation. A stale drain event from the previous
    // generation must not suppress or deliver data in the restarted session.
    if (!bridge.start())
        return 47;
    if (!feedOutgoing(bridge, 21))
        return 48;
    if (!waitUntil([&] { return rtpDeliveries == 2; }))
        return 49;
    bridge.stop();
    return 0;
}

int runConcurrentStop(const PsiMedia::PPayloadInfo &opus)
{
    int                        deliveries = 0;
    PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
    if (!bridge.isValid() || !bridge.setPayloads({ opus }, { opus }))
        return 50;
    bridge.setNetworkPacketHandler([&](const PsiMedia::PRtpPacket &packet) {
        if (packet.type == PsiMedia::PRtpPacket::Type::Rtp)
            ++deliveries;
    });
    if (!bridge.start())
        return 51;

    std::atomic<bool> send { true };
    std::thread       sender([&] {
        quint16 sequence = 100;
        while (send.load(std::memory_order_acquire)) {
            feedOutgoing(bridge, sequence++);
            std::this_thread::sleep_for(1ms);
        }
    });

    pumpFor(30ms);
    bridge.stop();
    send.store(false, std::memory_order_release);
    sender.join();

    const int stoppedAt = deliveries;
    pumpFor(50ms);
    if (deliveries != stoppedAt)
        return 52;
    return 0;
}

int runDeleteFromCallback(const PsiMedia::PPayloadInfo &opus)
{
    int      deliveries          = 0;
    bool     wrongCallbackThread = false;
    QThread *ownerThread         = QThread::currentThread();

    auto                                *bridge = new PsiMedia::RtpSessionBridge(QStringLiteral("audio"));
    QPointer<PsiMedia::RtpSessionBridge> guard(bridge);
    if (!bridge->isValid() || !bridge->setPayloads({ opus }, { opus })) {
        delete bridge;
        return 60;
    }

    bridge->setNetworkPacketHandler([bridge, &deliveries, &wrongCallbackThread, ownerThread](const auto &packet) {
        wrongCallbackThread |= QThread::currentThread() != ownerThread;
        if (packet.type != PsiMedia::PRtpPacket::Type::Rtp)
            return;
        ++deliveries;
        delete bridge;
    });
    if (!bridge->start()) {
        delete bridge;
        return 61;
    }
    if (!feedOutgoing(*bridge, 200)) {
        delete bridge;
        return 62;
    }

    if (!waitUntil([&] { return guard.isNull(); })) {
        delete bridge;
        return 63;
    }
    if (wrongCallbackThread || deliveries != 1)
        return 64;
    pumpFor(30ms);
    return 0;
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

    {
        PsiMedia::RtpSessionBridge bridge(QStringLiteral("audio"));
        if (!bridge.isValid()) {
            qCritical() << "failed to create compatibility-test bridge";
            return 30;
        }
        auto incompatible      = opus;
        incompatible.name      = QStringLiteral("PCMU");
        incompatible.clockrate = 8000;
        incompatible.channels  = 1;
        if (bridge.setPayloads({ opus }, { incompatible })) {
            qCritical() << "incompatible codecs sharing one PT were accepted";
            return 31;
        }
    }

    if (const int result = runScenario(opus, EarlyExit::None))
        return result;

    constexpr EarlyExit exits[] = {
        EarlyExit::AfterStart,        EarlyExit::AfterOutgoingFeed, EarlyExit::AfterOutgoingWait,
        EarlyExit::AfterIncomingFeed, EarlyExit::AfterIncomingWait, EarlyExit::AfterRtcpFeed,
        EarlyExit::AfterRtcpWait,     EarlyExit::AfterRtcpRequest,  EarlyExit::AfterRtcpOutputWait,
    };
    for (const auto point : exits) {
        if (const int result = runScenario(opus, point)) {
            qCritical() << "early-exit teardown regression failed at point" << int(point) << "with" << result;
            return 20 + result;
        }
    }

    if (const int result = runStopFromCallback(opus)) {
        qCritical() << "stop-from-callback regression failed with" << result;
        return result;
    }
    if (const int result = runConcurrentStop(opus)) {
        qCritical() << "concurrent-stop regression failed with" << result;
        return result;
    }
    if (const int result = runDeleteFromCallback(opus)) {
        qCritical() << "callback-destruction regression failed with" << result;
        return result;
    }

    qInfo() << "RTP/RTCP session bridge regression passed";
    return 0;
}
