/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "rtpsessionbridge.h"

#include "payloadinfo.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <utility>

namespace PsiMedia {
namespace {

    using CapsMap = QHash<int, GstCaps *>;

    constexpr int    MaxDeliveriesPerSlice = 32;
    constexpr qint64 MaxDeliverySliceMs    = 4;

    GstPad *requestPad(GstElement *element, const char *name)
    {
#if GST_CHECK_VERSION(1, 20, 0)
        return gst_element_request_pad_simple(element, name);
#else
        return gst_element_get_request_pad(element, name);
#endif
    }

    bool linkSourceToPad(GstElement *source, GstPad *sinkPad)
    {
        GstPad *sourcePad = gst_element_get_static_pad(source, "src");
        if (!sourcePad)
            return false;
        const bool linked = gst_pad_link(sourcePad, sinkPad) == GST_PAD_LINK_OK;
        gst_object_unref(sourcePad);
        return linked;
    }

    bool linkPadToSink(GstPad *sourcePad, GstElement *sink)
    {
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        if (!sinkPad)
            return false;
        const bool linked = gst_pad_link(sourcePad, sinkPad) == GST_PAD_LINK_OK;
        gst_object_unref(sinkPad);
        return linked;
    }

    void configureAppSrc(GstAppSrc *source, const char *mediaType, bool timestamp)
    {
        GstCaps *caps = gst_caps_new_empty_simple(mediaType);
        gst_app_src_set_caps(source, caps);
        gst_caps_unref(caps);
        g_object_set(G_OBJECT(source), "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", timestamp, nullptr);
    }

    void configureAppSink(GstAppSink *sink) { g_object_set(G_OBJECT(sink), "sync", FALSE, "async", FALSE, nullptr); }

    void unrefElement(GstElement *element)
    {
        if (element)
            gst_object_unref(element);
    }

    void unrefCapsMap(CapsMap &capsMap)
    {
        for (auto caps : std::as_const(capsMap))
            gst_caps_unref(caps);
        capsMap.clear();
    }

    GstCaps *capsForPayload(const PPayloadInfo &payload, const QString &media)
    {
        GstStructure *structure = payloadInfoToStructure(payload, media);
        if (!structure)
            return nullptr;
        GstCaps *caps = gst_caps_new_empty();
        gst_caps_append_structure(caps, structure);
        return caps;
    }

    bool payloadsCompatible(const PPayloadInfo &local, const PPayloadInfo &remote)
    {
        if (!local.name.isEmpty() && !remote.name.isEmpty()
            && local.name.compare(remote.name, Qt::CaseInsensitive) != 0)
            return false;
        if (local.clockrate >= 0 && remote.clockrate >= 0 && local.clockrate != remote.clockrate)
            return false;
        if (local.channels >= 0 && remote.channels >= 0 && local.channels != remote.channels)
            return false;
        return true;
    }

    PPayloadInfo commonPayload(const PPayloadInfo &primary, const PPayloadInfo *secondary)
    {
        PPayloadInfo common = primary;
        if (secondary) {
            if (common.name.isEmpty())
                common.name = secondary->name;
            if (common.clockrate < 0)
                common.clockrate = secondary->clockrate;
            if (common.channels < 0)
                common.channels = secondary->channels;
        }
        common.ptime    = -1;
        common.maxptime = -1;

        // Codec fmtp is direction-specific and must not leak into rtpsession's
        // shared PT map. Negotiated RTCP feedback, however, is session metadata:
        // rtpsession checks for these caps fields before turning a decoder
        // GstForceKeyUnit request into PLI/FIR.
        QList<PPayloadInfo::Parameter> feedback;
        const auto                     collectFeedback = [&feedback](const PPayloadInfo &payload) {
            for (const auto &parameter : payload.parameters) {
                if (!parameter.name.startsWith(QLatin1String("rtcp-fb-")))
                    continue;
                if (std::none_of(feedback.cbegin(), feedback.cend(),
                                                     [&](const auto &existing) { return existing.name == parameter.name; }))
                    feedback.append(parameter);
            }
        };
        collectFeedback(primary);
        if (secondary)
            collectFeedback(*secondary);
        common.parameters = std::move(feedback);
        return common;
    }

} // namespace

RtpSessionBridge::RtpSessionBridge(QString media) : QObject(nullptr), media_(std::move(media)) { build(); }

RtpSessionBridge::~RtpSessionBridge()
{
    ownerThread("destruction");
    cleanup();
}

bool RtpSessionBridge::ownerThread(const char *operation) const
{
    if (QThread::currentThread() == thread())
        return true;
    qWarning() << "RtpSessionBridge" << operation << "must run on its owner thread";
    return false;
}

bool RtpSessionBridge::build()
{
    GstElement *pipeline       = gst_pipeline_new(nullptr);
    GstElement *session        = gst_element_factory_make("rtpsession", nullptr);
    GstElement *sendRtpInput   = gst_element_factory_make("appsrc", nullptr);
    GstElement *recvRtpInput   = gst_element_factory_make("appsrc", nullptr);
    GstElement *recvRtcpInput  = gst_element_factory_make("appsrc", nullptr);
    GstElement *sendRtpOutput  = gst_element_factory_make("appsink", nullptr);
    GstElement *recvRtpOutput  = gst_element_factory_make("appsink", nullptr);
    GstElement *sendRtcpOutput = gst_element_factory_make("appsink", nullptr);

    if (!pipeline || !session || !sendRtpInput || !recvRtpInput || !recvRtcpInput || !sendRtpOutput || !recvRtpOutput
        || !sendRtcpOutput) {
        unrefElement(sendRtpInput);
        unrefElement(recvRtpInput);
        unrefElement(recvRtcpInput);
        unrefElement(sendRtpOutput);
        unrefElement(recvRtpOutput);
        unrefElement(sendRtcpOutput);
        unrefElement(session);
        unrefElement(pipeline);
        return false;
    }

    configureAppSrc(GST_APP_SRC(sendRtpInput), "application/x-rtp", false);
    configureAppSrc(GST_APP_SRC(recvRtpInput), "application/x-rtp", true);
    configureAppSrc(GST_APP_SRC(recvRtcpInput), "application/x-rtcp", true);
    configureAppSink(GST_APP_SINK(sendRtpOutput));
    configureAppSink(GST_APP_SINK(recvRtpOutput));
    configureAppSink(GST_APP_SINK(sendRtcpOutput));

    gst_bin_add_many(GST_BIN(pipeline), session, sendRtpInput, recvRtpInput, recvRtcpInput, sendRtpOutput,
                     recvRtpOutput, sendRtcpOutput, nullptr);

    pipeline_       = pipeline;
    bus_            = gst_element_get_bus(pipeline_);
    session_        = session;
    sendRtpInput_   = GST_APP_SRC(sendRtpInput);
    recvRtpInput_   = GST_APP_SRC(recvRtpInput);
    recvRtcpInput_  = GST_APP_SRC(recvRtcpInput);
    sendRtpOutput_  = GST_APP_SINK(sendRtpOutput);
    recvRtpOutput_  = GST_APP_SINK(recvRtpOutput);
    sendRtcpOutput_ = GST_APP_SINK(sendRtcpOutput);

    const auto fail = [this]() {
        cleanup();
        return false;
    };

    gst_util_set_object_arg(G_OBJECT(session_), "rtp-profile", "avpf");

    sendRtpSinkPad_  = requestPad(session_, "send_rtp_sink");
    recvRtpSinkPad_  = requestPad(session_, "recv_rtp_sink");
    recvRtcpSinkPad_ = requestPad(session_, "recv_rtcp_sink");
    sendRtcpSrcPad_  = requestPad(session_, "send_rtcp_src");
    if (!sendRtpSinkPad_ || !recvRtpSinkPad_ || !recvRtcpSinkPad_ || !sendRtcpSrcPad_)
        return fail();

    if (!linkSourceToPad(sendRtpInput, sendRtpSinkPad_) || !linkSourceToPad(recvRtpInput, recvRtpSinkPad_)
        || !linkSourceToPad(recvRtcpInput, recvRtcpSinkPad_))
        return fail();

    {
        GstPad *pad = gst_element_get_static_pad(session_, "send_rtp_src");
        if (!pad)
            return fail();
        const bool linked = linkPadToSink(pad, sendRtpOutput);
        gst_object_unref(pad);
        if (!linked)
            return fail();
    }
    {
        GstPad *pad = gst_element_get_static_pad(session_, "recv_rtp_src");
        if (!pad)
            return fail();
        const bool linked = linkPadToSink(pad, recvRtpOutput);
        gst_object_unref(pad);
        if (!linked)
            return fail();
    }
    if (!linkPadToSink(sendRtcpSrcPad_, sendRtcpOutput))
        return fail();

    g_signal_connect(session_, "request-pt-map", G_CALLBACK(requestPtMap), this);

    GstAppSinkCallbacks sendRtpCallbacks {};
    sendRtpCallbacks.new_sample = sendRtpReady;
    gst_app_sink_set_callbacks(sendRtpOutput_, &sendRtpCallbacks, this, nullptr);

    GstAppSinkCallbacks recvRtpCallbacks {};
    recvRtpCallbacks.new_sample = recvRtpReady;
    gst_app_sink_set_callbacks(recvRtpOutput_, &recvRtpCallbacks, this, nullptr);

    GstAppSinkCallbacks sendRtcpCallbacks {};
    sendRtcpCallbacks.new_sample = sendRtcpReady;
    gst_app_sink_set_callbacks(sendRtcpOutput_, &sendRtcpCallbacks, this, nullptr);

    GObject *internalSession = nullptr;
    g_object_get(session_, "internal-session", &internalSession, nullptr);
    if (!internalSession)
        return fail();
    g_signal_connect(internalSession, "on-receiving-rtcp", G_CALLBACK(receivingRtcp), this);
    g_object_unref(internalSession);

    return true;
}

void RtpSessionBridge::cleanup()
{
    ++busPollGeneration_;
    running_.store(false, std::memory_order_release);
    disableDeliveries();

    GstAppSinkCallbacks noCallbacks {};
    if (sendRtpOutput_)
        gst_app_sink_set_callbacks(sendRtpOutput_, &noCallbacks, nullptr, nullptr);
    if (recvRtpOutput_)
        gst_app_sink_set_callbacks(recvRtpOutput_, &noCallbacks, nullptr, nullptr);
    if (sendRtcpOutput_)
        gst_app_sink_set_callbacks(sendRtcpOutput_, &noCallbacks, nullptr, nullptr);

    if (session_) {
        g_signal_handlers_disconnect_by_data(session_, this);
        GObject *internalSession = nullptr;
        g_object_get(session_, "internal-session", &internalSession, nullptr);
        if (internalSession) {
            g_signal_handlers_disconnect_by_data(internalSession, this);
            g_object_unref(internalSession);
        }
    }

    if (pipeline_) {
        const GstStateChangeReturn setResult = gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (setResult == GST_STATE_CHANGE_ASYNC) {
            GstState                   current = GST_STATE_VOID_PENDING;
            GstState                   pending = GST_STATE_VOID_PENDING;
            const GstStateChangeReturn waitResult
                = gst_element_get_state(pipeline_, &current, &pending, 2 * GST_SECOND);
            if (waitResult == GST_STATE_CHANGE_ASYNC) {
                qWarning() << "RTP session bridge teardown timed out after 2s"
                           << "current=" << int(current) << "pending=" << int(pending);
            } else if (waitResult == GST_STATE_CHANGE_FAILURE) {
                qWarning() << "RTP session bridge teardown failed while waiting for NULL";
            }
        } else if (setResult == GST_STATE_CHANGE_FAILURE) {
            qWarning() << "RTP session bridge failed to enter NULL during teardown";
        }
    }

    if (session_) {
        if (sendRtpSinkPad_) {
            gst_element_release_request_pad(session_, sendRtpSinkPad_);
            gst_object_unref(sendRtpSinkPad_);
        }
        if (recvRtpSinkPad_) {
            gst_element_release_request_pad(session_, recvRtpSinkPad_);
            gst_object_unref(recvRtpSinkPad_);
        }
        if (recvRtcpSinkPad_) {
            gst_element_release_request_pad(session_, recvRtcpSinkPad_);
            gst_object_unref(recvRtcpSinkPad_);
        }
        if (sendRtcpSrcPad_) {
            gst_element_release_request_pad(session_, sendRtcpSrcPad_);
            gst_object_unref(sendRtcpSrcPad_);
        }
    }
    sendRtpSinkPad_ = recvRtpSinkPad_ = recvRtcpSinkPad_ = sendRtcpSrcPad_ = nullptr;

    if (bus_)
        gst_object_unref(bus_);
    bus_ = nullptr;
    if (pipeline_)
        gst_object_unref(pipeline_);
    pipeline_       = nullptr;
    session_        = nullptr;
    sendRtpInput_   = nullptr;
    recvRtpInput_   = nullptr;
    recvRtcpInput_  = nullptr;
    sendRtpOutput_  = nullptr;
    recvRtpOutput_  = nullptr;
    sendRtcpOutput_ = nullptr;

    QMutexLocker locker(&payloadMutex_);
    unrefCapsMap(localPayloadCaps_);
    unrefCapsMap(remotePayloadCaps_);
    unrefCapsMap(payloadCaps_);
}

bool RtpSessionBridge::setPayloads(const QList<PPayloadInfo> &local, const QList<PPayloadInfo> &remote)
{
    PayloadGroup group;
    group.endpointId = media_.toUtf8();
    group.media      = media_;
    group.local      = local;
    group.remote     = remote;
    return setPayloadGroups({ group });
}

bool RtpSessionBridge::setPayloadGroups(const QList<PayloadGroup> &groups)
{
    if (!ownerThread("setPayloadGroups"))
        return false;

    CapsMap nextLocal;
    CapsMap nextRemote;
    CapsMap nextCommon;

    const auto fail = [&]() {
        unrefCapsMap(nextLocal);
        unrefCapsMap(nextRemote);
        unrefCapsMap(nextCommon);
        return false;
    };

    QSet<QByteArray> endpointIds;

    const auto insertCaps = [&](const PPayloadInfo &payload, const QString &media, CapsMap &capsMap) {
        if (payload.id < 0 || payload.id > 127 || media.isEmpty())
            return false;
        GstCaps *caps = capsForPayload(payload, media);
        if (!caps)
            return false;
        const auto existing = capsMap.constFind(payload.id);
        if (existing == capsMap.cend()) {
            capsMap.insert(payload.id, caps);
            return true;
        }
        const bool same = gst_caps_is_equal(*existing, caps);
        gst_caps_unref(caps);
        return same;
    };

    const auto insertCommon = [&](const PPayloadInfo &primary, const PPayloadInfo *secondary, const QString &media) {
        if (secondary && !payloadsCompatible(primary, *secondary))
            return false;
        const PPayloadInfo common = commonPayload(primary, secondary);
        return insertCaps(common, media, nextCommon);
    };

    for (const auto &group : groups) {
        if (group.endpointId.isEmpty() || group.media.isEmpty() || endpointIds.contains(group.endpointId))
            return fail();
        endpointIds.insert(group.endpointId);

        QHash<int, PPayloadInfo> localInfo;
        QHash<int, PPayloadInfo> remoteInfo;

        for (const auto &payload : group.local) {
            if (!insertCaps(payload, group.media, nextLocal))
                return fail();
            const auto existing = localInfo.constFind(payload.id);
            if (existing != localInfo.cend() && !payloadsCompatible(existing.value(), payload))
                return fail();
            localInfo.insert(payload.id, payload);
        }

        for (const auto &payload : group.remote) {
            if (!insertCaps(payload, group.media, nextRemote))
                return fail();
            const auto existing = remoteInfo.constFind(payload.id);
            if (existing != remoteInfo.cend() && !payloadsCompatible(existing.value(), payload))
                return fail();
            remoteInfo.insert(payload.id, payload);
        }

        for (auto it = localInfo.cbegin(); it != localInfo.cend(); ++it) {
            const auto  remoteIt = remoteInfo.constFind(it.key());
            const auto *peer     = remoteIt == remoteInfo.cend() ? nullptr : &remoteIt.value();
            if (!insertCommon(it.value(), peer, group.media))
                return fail();
        }
        for (auto it = remoteInfo.cbegin(); it != remoteInfo.cend(); ++it) {
            if (localInfo.contains(it.key()))
                continue;
            if (!insertCommon(it.value(), nullptr, group.media))
                return fail();
        }
    }

    CapsMap oldLocal;
    CapsMap oldRemote;
    CapsMap oldCommon;
    {
        QMutexLocker locker(&payloadMutex_);
        oldLocal.swap(localPayloadCaps_);
        oldRemote.swap(remotePayloadCaps_);
        oldCommon.swap(payloadCaps_);
        localPayloadCaps_.swap(nextLocal);
        remotePayloadCaps_.swap(nextRemote);
        payloadCaps_.swap(nextCommon);
    }

    unrefCapsMap(oldLocal);
    unrefCapsMap(oldRemote);
    unrefCapsMap(oldCommon);

    if (session_)
        g_signal_emit_by_name(session_, "clear-pt-map");
    return true;
}

bool RtpSessionBridge::start()
{
    if (!ownerThread("start") || !pipeline_ || !bus_)
        return false;
    if (running_.load(std::memory_order_acquire))
        return true;

    // A stopped generation may leave state-change/EOS messages queued. They
    // are not runtime failures of the new generation.
    while (GstMessage *message = gst_bus_pop(bus_))
        gst_message_unref(message);

    const quint64 generation = ++busPollGeneration_;
    enableDeliveries();
    running_.store(true, std::memory_order_release);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        running_.store(false, std::memory_order_release);
        ++busPollGeneration_;
        disableDeliveries();
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        return false;
    }
    scheduleBusPoll(generation);
    return true;
}

void RtpSessionBridge::stop()
{
    if (!ownerThread("stop"))
        return;
    ++busPollGeneration_;
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    disableDeliveries();
    if (!pipeline_ || !wasRunning)
        return;
    const GstStateChangeReturn setResult = gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (setResult == GST_STATE_CHANGE_ASYNC) {
        GstState                   current    = GST_STATE_VOID_PENDING;
        GstState                   pending    = GST_STATE_VOID_PENDING;
        const GstStateChangeReturn waitResult = gst_element_get_state(pipeline_, &current, &pending, 2 * GST_SECOND);
        if (waitResult == GST_STATE_CHANGE_ASYNC) {
            qWarning() << "RTP session bridge teardown timed out after 2s"
                       << "current=" << int(current) << "pending=" << int(pending);
        } else if (waitResult == GST_STATE_CHANGE_FAILURE) {
            qWarning() << "RTP session bridge teardown failed while waiting for NULL";
        }
    } else if (setResult == GST_STATE_CHANGE_FAILURE) {
        qWarning() << "RTP session bridge failed to enter NULL during teardown";
    }
}

void RtpSessionBridge::scheduleBusPoll(quint64 generation)
{
    QTimer::singleShot(10, this, [this, generation]() {
        if (generation == busPollGeneration_ && running_.load(std::memory_order_acquire))
            pollBus(generation);
    });
}

void RtpSessionBridge::pollBus(quint64 generation)
{
    if (generation != busPollGeneration_ || !running_.load(std::memory_order_acquire) || !bus_)
        return;

    bool runtimeError = false;
    while (GstMessage *message = gst_bus_pop(bus_)) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar  *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            qWarning() << "RTP session bridge GStreamer error:"
                       << (error && error->message ? error->message : "unknown");
            if (debug && *debug)
                qWarning() << "RTP session bridge debug:" << debug;
            if (error)
                g_error_free(error);
            g_free(debug);
            runtimeError = true;
        }
        gst_message_unref(message);
        if (runtimeError)
            break;
    }

    if (!runtimeError) {
        scheduleBusPoll(generation);
        return;
    }

    // Retire this generation before invoking external code. The handler can
    // synchronously clean up or delete the owning GstRtpSessionContext.
    ++busPollGeneration_;
    running_.store(false, std::memory_order_release);
    disableDeliveries();
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_element_get_state(pipeline_, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    }

    const auto handler = runtimeErrorHandler_;
    if (handler)
        handler();
}

GstClockTime RtpSessionBridge::runningTime() const
{
    if (!pipeline_)
        return GST_CLOCK_TIME_NONE;

    GstClock *clock = gst_element_get_clock(pipeline_);
    if (!clock)
        return GST_CLOCK_TIME_NONE;

    const GstClockTime now  = gst_clock_get_time(clock);
    const GstClockTime base = gst_element_get_base_time(pipeline_);
    gst_object_unref(clock);
    if (!GST_CLOCK_TIME_IS_VALID(now) || !GST_CLOCK_TIME_IS_VALID(base) || now < base)
        return GST_CLOCK_TIME_NONE;
    return now - base;
}

GstFlowReturn RtpSessionBridge::sendRtp(GstBuffer *buffer)
{
    if (!running_.load(std::memory_order_acquire) || !sendRtpInput_ || !buffer)
        return GST_FLOW_FLUSHING;
    return gst_app_src_push_buffer(sendRtpInput_, gst_buffer_ref(buffer));
}

GstFlowReturn RtpSessionBridge::sendRtp(GstBuffer *buffer, GstClockTime presentationAge)
{
    if (!buffer)
        return GST_FLOW_ERROR;

    GstBuffer *mapped = gst_buffer_copy(buffer);
    if (!mapped)
        return GST_FLOW_ERROR;

    const GstClockTime now = runningTime();
    if (GST_CLOCK_TIME_IS_VALID(now)) {
        const GstClockTime mappedPts
            = GST_CLOCK_TIME_IS_VALID(presentationAge) ? (presentationAge <= now ? now - presentationAge : 0) : now;
        GST_BUFFER_PTS(mapped) = mappedPts;
        GST_BUFFER_DTS(mapped) = mappedPts;
    }

    const auto flow = sendRtp(mapped);
    gst_buffer_unref(mapped);
    return flow;
}

GstFlowReturn RtpSessionBridge::receivePacket(const PRtpPacket &packet)
{
    if (!running_.load(std::memory_order_acquire))
        return GST_FLOW_FLUSHING;
    return packet.type == PRtpPacket::Type::Rtp ? pushRaw(recvRtpInput_, packet.rawValue)
                                                : pushRaw(recvRtcpInput_, packet.rawValue);
}

void RtpSessionBridge::setNetworkPacketHandler(NetworkPacketHandler handler)
{
    if (!ownerThread("setNetworkPacketHandler"))
        return;
    networkPacketHandler_ = std::move(handler);
}

void RtpSessionBridge::setMediaPacketHandler(MediaPacketHandler handler)
{
    if (!ownerThread("setMediaPacketHandler"))
        return;
    mediaPacketHandler_ = std::move(handler);
}

void RtpSessionBridge::setRuntimeErrorHandler(RuntimeErrorHandler handler)
{
    if (!ownerThread("setRuntimeErrorHandler"))
        return;
    runtimeErrorHandler_ = std::move(handler);
}

bool RtpSessionBridge::requestRtcp(guint64 maxDelay)
{
    if (!ownerThread("requestRtcp") || !running_.load(std::memory_order_acquire) || !session_)
        return false;

    GObject *internalSession = nullptr;
    g_object_get(session_, "internal-session", &internalSession, nullptr);
    if (!internalSession)
        return false;

    gboolean scheduled = FALSE;
    g_signal_emit_by_name(internalSession, "send-rtcp-full", maxDelay, &scheduled);
    g_object_unref(internalSession);
    return scheduled;
}

bool RtpSessionBridge::requestRemoteKeyframe(quint32 ssrc, quint8 payloadType)
{
    if (!ownerThread("requestRemoteKeyframe") || !running_.load(std::memory_order_acquire) || !session_ || !ssrc
        || payloadType > 127)
        return false;

    GstPad *pad = gst_element_get_static_pad(session_, "recv_rtp_src");
    if (!pad)
        return false;

    GstStructure *structure
        = gst_structure_new("GstForceKeyUnit", "ssrc", G_TYPE_UINT, guint(ssrc), "payload", G_TYPE_UINT,
                            guint(payloadType), "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr);
    GstEvent  *event   = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, structure);
    const bool handled = gst_pad_send_event(pad, event) != FALSE;
    gst_object_unref(pad);
    return handled;
}

void RtpSessionBridge::setRtcpMinimumInterval(guint64 interval)
{
    if (!ownerThread("setRtcpMinimumInterval"))
        return;
    if (session_)
        g_object_set(session_, "rtcp-min-interval", interval, nullptr);
}

GstCaps *RtpSessionBridge::requestPtMap(GstElement *, guint pt, gpointer data)
{
    return static_cast<RtpSessionBridge *>(data)->payloadCaps(pt);
}

GstFlowReturn RtpSessionBridge::sendRtpReady(GstAppSink *sink, gpointer data)
{
    return static_cast<RtpSessionBridge *>(data)->pullNetworkPacket(sink, PRtpPacket::Type::Rtp);
}

GstFlowReturn RtpSessionBridge::recvRtpReady(GstAppSink *sink, gpointer data)
{
    return static_cast<RtpSessionBridge *>(data)->pullMediaPacket(sink);
}

GstFlowReturn RtpSessionBridge::sendRtcpReady(GstAppSink *sink, gpointer data)
{
    return static_cast<RtpSessionBridge *>(data)->pullNetworkPacket(sink, PRtpPacket::Type::Rtcp);
}

void RtpSessionBridge::receivingRtcp(GObject *, GstBuffer *, gpointer data)
{
    auto *bridge = static_cast<RtpSessionBridge *>(data);
    if (bridge->running_.load(std::memory_order_acquire))
        bridge->receivedRtcpPackets_.fetch_add(1);
}

GstCaps *RtpSessionBridge::payloadCaps(guint pt)
{
    QMutexLocker locker(&payloadMutex_);
    auto         it = payloadCaps_.constFind(int(pt));
    return it == payloadCaps_.cend() ? nullptr : gst_caps_ref(*it);
}

quint64 RtpSessionBridge::deliveryGeneration() const
{
    QMutexLocker locker(&deliveryMutex_);
    return deliveriesEnabled_ ? generation_ : 0;
}

void RtpSessionBridge::enableDeliveries()
{
    QMutexLocker locker(&deliveryMutex_);
    clearDeliveryQueuesLocked();
    ++generation_;
    if (generation_ == 0)
        ++generation_;
    scheduledDeliveryGeneration_ = 0;
    deliveriesEnabled_           = true;
}

void RtpSessionBridge::disableDeliveries()
{
    QMutexLocker locker(&deliveryMutex_);
    deliveriesEnabled_ = false;
    ++generation_;
    if (generation_ == 0)
        ++generation_;
    scheduledDeliveryGeneration_ = 0;
    clearDeliveryQueuesLocked();
}

void RtpSessionBridge::clearDeliveryQueuesLocked()
{
    networkQueue_.clear();
    while (!mediaQueue_.isEmpty()) {
        const auto item = mediaQueue_.dequeue();
        if (item.buffer)
            gst_buffer_unref(item.buffer);
    }
}

void RtpSessionBridge::enqueueNetworkPacket(quint64 generation, PRtpPacket packet)
{
    if (!generation)
        return;
    QMutexLocker locker(&deliveryMutex_);
    if (!deliveriesEnabled_ || generation != generation_)
        return;
    if (networkQueue_.size() >= MaxQueuedNetworkPackets)
        networkQueue_.dequeue();
    networkQueue_.enqueue({ generation, std::move(packet) });
    scheduleDeliveryLocked(generation);
}

void RtpSessionBridge::enqueueMediaPacket(quint64 generation, GstBuffer *buffer)
{
    if (!generation || !buffer)
        return;
    QMutexLocker locker(&deliveryMutex_);
    if (!deliveriesEnabled_ || generation != generation_)
        return;
    if (mediaQueue_.size() >= MaxQueuedMediaPackets) {
        const auto dropped = mediaQueue_.dequeue();
        if (dropped.buffer)
            gst_buffer_unref(dropped.buffer);
    }
    mediaQueue_.enqueue({ generation, gst_buffer_ref(buffer) });
    scheduleDeliveryLocked(generation);
}

void RtpSessionBridge::scheduleDeliveryLocked(quint64 generation)
{
    if (scheduledDeliveryGeneration_ == generation)
        return;
    scheduledDeliveryGeneration_ = generation;
    QTimer::singleShot(0, this, [this, generation]() { drainDeliveries(generation); });
}

void RtpSessionBridge::drainDeliveries(quint64 generation)
{
    if (!ownerThread("packet delivery"))
        return;

    QElapsedTimer slice;
    slice.start();
    bool preferNetwork = true;
    int  delivered     = 0;

    for (;;) {
        QueuedNetworkPacket network;
        QueuedMediaPacket   media;
        bool                haveNetwork = false;
        bool                haveMedia   = false;

        {
            QMutexLocker locker(&deliveryMutex_);
            if (!deliveriesEnabled_ || generation != generation_) {
                if (scheduledDeliveryGeneration_ == generation)
                    scheduledDeliveryGeneration_ = 0;
                return;
            }

            if (!networkQueue_.isEmpty() && (preferNetwork || mediaQueue_.isEmpty())) {
                network       = networkQueue_.dequeue();
                haveNetwork   = true;
                preferNetwork = false;
            } else if (!mediaQueue_.isEmpty()) {
                media         = mediaQueue_.dequeue();
                haveMedia     = true;
                preferNetwork = true;
            } else if (!networkQueue_.isEmpty()) {
                network       = networkQueue_.dequeue();
                haveNetwork   = true;
                preferNetwork = false;
            } else {
                if (scheduledDeliveryGeneration_ == generation)
                    scheduledDeliveryGeneration_ = 0;
                return;
            }
        }

        if (haveNetwork) {
            const auto handler = networkPacketHandler_;
            if (handler) {
                QPointer<RtpSessionBridge> guard(this);
                handler(network.packet);
                if (!guard)
                    return;
            }
        } else if (haveMedia) {
            const auto                 handler = mediaPacketHandler_;
            QPointer<RtpSessionBridge> guard(this);
            if (handler)
                handler(media.buffer);
            if (media.buffer)
                gst_buffer_unref(media.buffer);
            if (!guard)
                return;
        }

        ++delivered;
        if (delivered < MaxDeliveriesPerSlice && slice.elapsed() < MaxDeliverySliceMs)
            continue;

        // A continuously replenished queue must not monopolize the QObject
        // owner thread. Keep this generation scheduled, but yield so stop(),
        // timers and GstBus error polling get a turn before the next slice.
        QMutexLocker locker(&deliveryMutex_);
        if (!deliveriesEnabled_ || generation != generation_) {
            if (scheduledDeliveryGeneration_ == generation)
                scheduledDeliveryGeneration_ = 0;
            return;
        }
        if (networkQueue_.isEmpty() && mediaQueue_.isEmpty()) {
            if (scheduledDeliveryGeneration_ == generation)
                scheduledDeliveryGeneration_ = 0;
            return;
        }
        if (scheduledDeliveryGeneration_ == generation)
            scheduledDeliveryGeneration_ = 0;
        scheduleDeliveryLocked(generation);
        return;
    }
}

GstFlowReturn RtpSessionBridge::pullNetworkPacket(GstAppSink *sink, PRtpPacket::Type type)
{
    const quint64 generation = deliveryGeneration();
    GstSample    *sample     = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_ERROR;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    if (!generation) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    const auto size = gst_buffer_get_size(buffer);
    PRtpPacket packet;
    packet.rawValue.resize(int(size));
    packet.type = type;
    if (size && gst_buffer_extract(buffer, 0, packet.rawValue.data(), size) != size) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    gst_sample_unref(sample);
    enqueueNetworkPacket(generation, std::move(packet));
    return GST_FLOW_OK;
}

GstFlowReturn RtpSessionBridge::pullMediaPacket(GstAppSink *sink)
{
    const quint64 generation = deliveryGeneration();
    GstSample    *sample     = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_ERROR;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    if (generation)
        enqueueMediaPacket(generation, buffer);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

GstFlowReturn RtpSessionBridge::pushRaw(GstAppSrc *source, const QByteArray &data)
{
    if (!source || data.isEmpty())
        return GST_FLOW_ERROR;
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, gsize(data.size()), nullptr);
    if (!buffer)
        return GST_FLOW_ERROR;
    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }
    std::memcpy(info.data, data.constData(), size_t(data.size()));
    gst_buffer_unmap(buffer, &info);
    return gst_app_src_push_buffer(source, buffer);
}

} // namespace PsiMedia
