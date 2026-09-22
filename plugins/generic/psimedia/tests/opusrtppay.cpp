/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "bins.h"

#include <QCoreApplication>
#include <QDebug>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace {

constexpr int NegotiatedPayloadType = 109;
constexpr int RawSampleRate         = 44100;
constexpr int RawChannels           = 1;
constexpr int OpusRtpClockRate      = 48000;
constexpr int OpusRtpChannels       = 2;

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *source   = gst_element_factory_make("audiotestsrc", nullptr);
    GstElement *filter   = gst_element_factory_make("capsfilter", nullptr);
    GstElement *encoder  = PsiMedia::bins_audioenc_create(QStringLiteral("opus"), NegotiatedPayloadType);
    GstElement *sink     = gst_element_factory_make("appsink", nullptr);
    if (!pipeline || !source || !filter || !encoder || !sink) {
        qCritical() << "Failed to create Opus RTP test pipeline";
        if (pipeline)
            gst_object_unref(pipeline);
        else {
            if (source)
                gst_object_unref(source);
            if (filter)
                gst_object_unref(filter);
            if (encoder)
                gst_object_unref(encoder);
            if (sink)
                gst_object_unref(sink);
        }
        return 1;
    }

    GstCaps *rawCaps = gst_caps_new_simple("audio/x-raw", "rate", G_TYPE_INT, RawSampleRate, "channels", G_TYPE_INT,
                                           RawChannels, nullptr);
    g_object_set(G_OBJECT(filter), "caps", rawCaps, nullptr);
    gst_caps_unref(rawCaps);

    g_object_set(G_OBJECT(source), "num-buffers", 20, nullptr);
    g_object_set(G_OBJECT(sink), "sync", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, filter, encoder, sink, nullptr);
    if (!gst_element_link_many(source, filter, encoder, sink, nullptr)) {
        qCritical() << "Failed to link Opus RTP test pipeline";
        gst_object_unref(pipeline);
        return 2;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        qCritical() << "Failed to start Opus RTP test pipeline";
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return 3;
    }

    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
    if (!sample) {
        qCritical() << "Timed out waiting for an Opus RTP packet";
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return 4;
    }

    bool ok = true;

    GstPad  *encoderSink = gst_element_get_static_pad(encoder, "sink");
    GstCaps *inputCaps   = encoderSink ? gst_pad_get_current_caps(encoderSink) : nullptr;
    if (encoderSink)
        gst_object_unref(encoderSink);
    const auto *inputStructure = inputCaps ? gst_caps_get_structure(inputCaps, 0) : nullptr;
    gint        inputRate      = -1;
    gint        inputChannels  = -1;
    if (!inputStructure || !gst_structure_get_int(inputStructure, "rate", &inputRate)
        || !gst_structure_get_int(inputStructure, "channels", &inputChannels) || inputRate != RawSampleRate
        || inputChannels != RawChannels) {
        gchar *capsString = inputCaps ? gst_caps_to_string(inputCaps) : nullptr;
        qCritical() << "Unexpected raw audio caps at encoder input" << capsString;
        g_free(capsString);
        ok = false;
    }
    if (inputCaps)
        gst_caps_unref(inputCaps);

    GstCaps    *caps   = gst_sample_get_caps(sample);
    GstBuffer  *buffer = gst_sample_get_buffer(sample);
    const auto *s      = caps ? gst_caps_get_structure(caps, 0) : nullptr;

    gint         clockRate      = -1;
    const gchar *encoding       = s ? gst_structure_get_string(s, "encoding-name") : nullptr;
    const gchar *encodingParams = s ? gst_structure_get_string(s, "encoding-params") : nullptr;
    bool         channelsOk     = false;
    const int    rtpChannels    = encodingParams ? QString::fromLatin1(encodingParams).toInt(&channelsOk) : -1;
    if (!s || !encoding || QString::fromLatin1(encoding).compare(QStringLiteral("OPUS"), Qt::CaseInsensitive) != 0
        || !gst_structure_get_int(s, "clock-rate", &clockRate) || clockRate != OpusRtpClockRate || !channelsOk
        || rtpChannels != OpusRtpChannels) {
        gchar *capsString = caps ? gst_caps_to_string(caps) : nullptr;
        qCritical() << "Unexpected Opus RTP caps" << capsString;
        g_free(capsString);
        ok = false;
    }

    guint8 header[2] = {};
    if (!buffer || gst_buffer_get_size(buffer) < sizeof(header)
        || gst_buffer_extract(buffer, 0, header, sizeof(header)) != sizeof(header) || (header[0] >> 6) != 2
        || (header[1] & 0x7f) != NegotiatedPayloadType) {
        qCritical() << "rtpopuspay did not emit negotiated PT" << NegotiatedPayloadType;
        ok = false;
    }

    gst_sample_unref(sample);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_element_get_state(pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
    gst_object_unref(pipeline);

    if (!ok)
        return 5;

    qInfo() << "Raw audio" << RawSampleRate << "Hz /" << RawChannels << "channel became Opus RTP" << OpusRtpClockRate
            << "Hz /" << OpusRtpChannels << "channels with PT" << NegotiatedPayloadType;
    return 0;
}
