/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef RTPSESSIONBRIDGE_H
#define RTPSESSIONBRIDGE_H

#include "psimediaprovider.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>

#include <atomic>
#include <chrono>
#include <functional>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <utility>

namespace PsiMedia {

/**
 * Shared RFC 3550 session state for one media type.
 *
 * The bridge deliberately has no transport topology. Callers provide and
 * receive semantic RTP/RTCP packets; ICE components, UDP ports, RTCP mux and
 * BUNDLE remain responsibilities of the layer above psimedia.
 *
 * The construction thread owns the control plane: payload configuration,
 * handler replacement, start/stop, RTCP scheduling and destruction must run
 * there. sendRtp() and receivePacket() may race stop() while the bridge remains
 * alive. GStreamer streaming threads never invoke user handlers directly;
 * packet delivery is queued back to the owner thread.
 */
class RtpSessionBridge : public QObject {
public:
    using NetworkPacketHandler = std::function<void(const PRtpPacket &)>;
    using RuntimeErrorHandler  = std::function<void()>;
    // The buffer is borrowed for the duration of the callback. A consumer that
    // queues it or passes ownership to appsrc must take its own reference.
    using MediaPacketHandler = std::function<void(GstBuffer *)>;

    struct DeliveryQueueStats {
        int     networkPackets      = 0;
        int     mediaPackets        = 0;
        quint64 networkBytes        = 0;
        quint64 mediaBytes          = 0;
        quint64 networkByteDrops    = 0;
        quint64 mediaByteDrops      = 0;
        quint64 networkExpiredDrops = 0;
        quint64 mediaExpiredDrops   = 0;
    };

    explicit RtpSessionBridge(QString media);
    ~RtpSessionBridge() override;

    RtpSessionBridge(const RtpSessionBridge &)            = delete;
    RtpSessionBridge &operator=(const RtpSessionBridge &) = delete;

    bool isValid() const { return pipeline_ != nullptr; }

    struct PayloadGroup {
        QByteArray          endpointId;
        QString             media;
        QList<PPayloadInfo> local;
        QList<PPayloadInfo> remote;
    };

    /** Update the legacy single-media payload map. Owner thread only; safe while running. */
    bool setPayloads(const QList<PPayloadInfo> &local, const QList<PPayloadInfo> &remote);

    /**
     * Configure the payload map for one RTP session shared by several media
     * endpoints. Payload types reused by two endpoints must resolve to identical
     * RTP caps because rtpsession's request-pt-map is keyed only by PT.
     */
    bool setPayloadGroups(const QList<PayloadGroup> &groups);

    /** Owner-thread control-plane operations. */
    bool start();
    void stop();

    /** Feed encoded RTP already expressed in this bridge's running-time domain. */
    GstFlowReturn sendRtp(GstBuffer *buffer);

    /**
     * Feed encoded RTP from another pipeline.
     *
     * presentationAge is measured in that producer's running-time domain at
     * callback time. The bridge maps the age into its own running-time domain,
     * preserving encoder/queue delay without assuming common pipeline base-time.
     * GST_CLOCK_TIME_NONE falls back to arrival-time.
     */
    GstFlowReturn sendRtp(GstBuffer *buffer, GstClockTime presentationAge);

    /**
     * Feed an RTP packet from a legacy byte-oriented sender edge.
     *
     * The bridge stamps the packet with its own pipeline running time before
     * handing it to rtpsession. This keeps RTCP sender-report timing valid even
     * when the legacy producer has already discarded the original GstBuffer.
     */
    GstFlowReturn sendRtp(const PRtpPacket &packet)
    {
        if (packet.type != PRtpPacket::Type::Rtp || packet.rawValue.isEmpty())
            return GST_FLOW_ERROR;

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, gsize(packet.rawValue.size()), nullptr);
        if (!buffer)
            return GST_FLOW_ERROR;
        if (gst_buffer_fill(buffer, 0, packet.rawValue.constData(), gsize(packet.rawValue.size()))
            != gsize(packet.rawValue.size())) {
            gst_buffer_unref(buffer);
            return GST_FLOW_ERROR;
        }

        if (pipeline_) {
            GstClock *clock = gst_element_get_clock(pipeline_);
            if (clock) {
                const GstClockTime now  = gst_clock_get_time(clock);
                const GstClockTime base = gst_element_get_base_time(pipeline_);
                if (GST_CLOCK_TIME_IS_VALID(now) && GST_CLOCK_TIME_IS_VALID(base) && now >= base) {
                    GST_BUFFER_PTS(buffer) = now - base;
                    GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
                }
                gst_object_unref(clock);
            }
        }

        const auto flow = sendRtp(buffer);
        gst_buffer_unref(buffer);
        return flow;
    }

    /** Feed authenticated RTP or RTCP received from the Jingle layer. */
    GstFlowReturn receivePacket(const PRtpPacket &packet);

    /** Handler replacement is serialized on the owner thread. */
    void setNetworkPacketHandler(NetworkPacketHandler handler);
    void setMediaPacketHandler(MediaPacketHandler handler);
    void setRuntimeErrorHandler(RuntimeErrorHandler handler);

    /** Request an early RTCP report/feedback packet within maxDelay ns. Owner thread only. */
    bool requestRtcp(guint64 maxDelay = 0);

    /**
     * Forward a decoder keyframe request to this RTP session. If the negotiated
     * PT caps contain rtcp-fb-nack-pli, rtpsession schedules a PLI for @ssrc.
     * Owner thread only.
     */
    bool requestRemoteKeyframe(quint32 ssrc, quint8 payloadType);

    quint64 receivedRtcpPackets() const { return receivedRtcpPackets_.load(); }

    /**
     * Return a caller-owned copy of rtpsession's diagnostic statistics.
     * The caller must release a non-null result with gst_structure_free().
     * Owner thread only.
     */
    GstStructure *sessionStats() const
    {
        if (!ownerThread("sessionStats") || !session_)
            return nullptr;
        GstStructure *stats = nullptr;
        g_object_get(session_, "stats", &stats, nullptr);
        return stats;
    }

    /** Snapshot bounded-delivery diagnostics. Safe from any thread. */
    DeliveryQueueStats deliveryQueueStats() const
    {
        QMutexLocker locker(&deliveryMutex_);
        return { networkQueue_.rawSize(),      mediaQueue_.rawSize(),     networkQueue_.bytes(),
                 mediaQueue_.bytes(),          networkQueue_.byteDrops(), mediaQueue_.byteDrops(),
                 networkQueue_.expiredDrops(), mediaQueue_.expiredDrops() };
    }

    static constexpr int     maxQueuedNetworkPackets() { return MaxQueuedNetworkPackets; }
    static constexpr int     maxQueuedMediaPackets() { return MaxQueuedMediaPackets; }
    static constexpr quint64 maxQueuedNetworkBytes() { return MaxQueuedNetworkBytes; }
    static constexpr quint64 maxQueuedMediaBytes() { return MaxQueuedMediaBytes; }
    static constexpr qint64  maxQueuedPacketAgeMs() { return MaxQueuedPacketAgeMs; }

    /** Test/diagnostic tuning; owner thread only. */
    void setRtcpMinimumInterval(guint64 interval);

private:
    struct QueuedNetworkPacket {
        quint64                               generation = 0;
        PRtpPacket                            packet;
        std::chrono::steady_clock::time_point enqueuedAt = std::chrono::steady_clock::now();

        quint64 byteSize() const { return quint64(packet.rawValue.size()); }
        void    release() { }
    };
    struct QueuedMediaPacket {
        quint64                               generation = 0;
        GstBuffer                            *buffer     = nullptr;
        std::chrono::steady_clock::time_point enqueuedAt = std::chrono::steady_clock::now();

        quint64 byteSize() const { return buffer ? quint64(gst_buffer_get_size(buffer)) : 0; }
        void    release()
        {
            if (buffer) {
                gst_buffer_unref(buffer);
                buffer = nullptr;
            }
        }
    };

    static constexpr int     MaxQueuedNetworkPackets = 256;
    static constexpr int     MaxQueuedMediaPackets   = 128;
    static constexpr quint64 MaxQueuedNetworkBytes   = 512 * 1024;
    static constexpr quint64 MaxQueuedMediaBytes     = 512 * 1024;
    static constexpr qint64  MaxQueuedPacketAgeMs    = 1000;

    template <typename Item, int MaxPackets, quint64 MaxBytes, qint64 MaxAgeMs> class DeliveryQueue {
    public:
        ~DeliveryQueue() { clear(); }

        int size()
        {
            purgeExpired();
            return queue_.size();
        }

        bool isEmpty()
        {
            purgeExpired();
            return queue_.isEmpty();
        }

        void enqueue(Item item)
        {
            purgeExpired();
            const quint64 size = item.byteSize();
            if (size > MaxBytes) {
                ++byteDrops_;
                item.release();
                return;
            }
            while (!queue_.isEmpty() && (queue_.size() >= MaxPackets || bytes_ + size > MaxBytes)) {
                if (bytes_ + size > MaxBytes)
                    ++byteDrops_;
                dropFront(false);
            }
            queue_.enqueue(std::move(item));
            bytes_ += size;
        }

        Item dequeue()
        {
            Item          item = queue_.dequeue();
            const quint64 size = item.byteSize();
            bytes_             = size > bytes_ ? 0 : bytes_ - size;
            return item;
        }

        void clear()
        {
            while (!queue_.isEmpty())
                dropFront(false);
            bytes_ = 0;
        }

        int     rawSize() const { return queue_.size(); }
        quint64 bytes() const { return bytes_; }
        quint64 byteDrops() const { return byteDrops_; }
        quint64 expiredDrops() const { return expiredDrops_; }

    private:
        void purgeExpired()
        {
            const auto now    = std::chrono::steady_clock::now();
            const auto maxAge = std::chrono::milliseconds(MaxAgeMs);
            while (!queue_.isEmpty() && now - queue_.head().enqueuedAt > maxAge)
                dropFront(true);
        }

        void dropFront(bool expired)
        {
            Item          item = queue_.dequeue();
            const quint64 size = item.byteSize();
            bytes_             = size > bytes_ ? 0 : bytes_ - size;
            if (expired)
                ++expiredDrops_;
            item.release();
        }

        QQueue<Item> queue_;
        quint64      bytes_        = 0;
        quint64      byteDrops_    = 0;
        quint64      expiredDrops_ = 0;
    };

    static GstCaps      *requestPtMap(GstElement *session, guint pt, gpointer data);
    static GstFlowReturn sendRtpReady(GstAppSink *sink, gpointer data);
    static GstFlowReturn recvRtpReady(GstAppSink *sink, gpointer data);
    static GstFlowReturn sendRtcpReady(GstAppSink *sink, gpointer data);
    static void          receivingRtcp(GObject *session, GstBuffer *buffer, gpointer data);

    bool          build();
    void          cleanup();
    bool          ownerThread(const char *operation) const;
    GstClockTime  runningTime() const;
    void          scheduleBusPoll(quint64 generation);
    void          pollBus(quint64 generation);
    GstCaps      *payloadCaps(guint pt);
    GstFlowReturn pullNetworkPacket(GstAppSink *sink, PRtpPacket::Type type);
    GstFlowReturn pullMediaPacket(GstAppSink *sink);
    GstFlowReturn pushRaw(GstAppSrc *source, const QByteArray &data);

    quint64 deliveryGeneration() const;
    void    enableDeliveries();
    void    disableDeliveries();
    void    clearDeliveryQueuesLocked();
    void    enqueueNetworkPacket(quint64 generation, PRtpPacket packet);
    void    enqueueMediaPacket(quint64 generation, GstBuffer *buffer);
    void    scheduleDeliveryLocked(quint64 generation);
    void    drainDeliveries(quint64 generation);

    friend struct RtpSessionBridgeTestAccess;

    QString media_;

    GstElement *pipeline_       = nullptr;
    GstBus     *bus_            = nullptr;
    GstElement *session_        = nullptr;
    GstAppSrc  *sendRtpInput_   = nullptr;
    GstAppSrc  *recvRtpInput_   = nullptr;
    GstAppSrc  *recvRtcpInput_  = nullptr;
    GstAppSink *sendRtpOutput_  = nullptr;
    GstAppSink *recvRtpOutput_  = nullptr;
    GstAppSink *sendRtcpOutput_ = nullptr;

    GstPad *sendRtpSinkPad_  = nullptr;
    GstPad *recvRtpSinkPad_  = nullptr;
    GstPad *recvRtcpSinkPad_ = nullptr;
    GstPad *sendRtcpSrcPad_  = nullptr;

    QMutex payloadMutex_;
    // Keep the negotiated direction-specific caps intact. rtpsession's
    // request-pt-map callback uses payloadCaps_, a normalized map containing
    // only the codec identity/timing fields common to both directions.
    QHash<int, GstCaps *> localPayloadCaps_;
    QHash<int, GstCaps *> remotePayloadCaps_;
    QHash<int, GstCaps *> payloadCaps_;

    mutable QMutex deliveryMutex_;
    DeliveryQueue<QueuedNetworkPacket, MaxQueuedNetworkPackets, MaxQueuedNetworkBytes, MaxQueuedPacketAgeMs>
                                                                                                       networkQueue_;
    DeliveryQueue<QueuedMediaPacket, MaxQueuedMediaPackets, MaxQueuedMediaBytes, MaxQueuedPacketAgeMs> mediaQueue_;
    quint64 scheduledDeliveryGeneration_ = 0;
    quint64 generation_                  = 0;
    bool    deliveriesEnabled_           = false;

    NetworkPacketHandler networkPacketHandler_;
    MediaPacketHandler   mediaPacketHandler_;
    RuntimeErrorHandler  runtimeErrorHandler_;
    quint64              busPollGeneration_ = 0;
    std::atomic<quint64> receivedRtcpPackets_ { 0 };
    std::atomic<bool>    running_ { false };
};

} // namespace PsiMedia

#endif
