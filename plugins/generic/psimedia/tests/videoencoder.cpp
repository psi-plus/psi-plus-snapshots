// SPDX-License-Identifier: LGPL-2.1-or-later
#include "bins.h"
#include <QCoreApplication>
#include <QSemaphore>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <limits>
#include <vector>

namespace {
void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
void finish(GstElement *pipeline)
{
    auto *bus = gst_element_get_bus(pipeline);
    auto *message
        = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    check(message, "video pipeline timed out");
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr;
        gchar  *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        qFatal("video pipeline failed: %s (%s)", error->message, debug ? debug : "");
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_message_unref(message);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
}
void queueUnderPressure(bool live)
{
    auto *pipeline = gst_pipeline_new(nullptr);
    auto *source   = gst_element_factory_make("appsrc", nullptr);
    auto *queue    = PsiMedia::bins_videoqueue_create(nullptr, live);
    auto *sink     = gst_element_factory_make("appsink", nullptr);
    check(pipeline && source && queue && sink, "could not create queue regression pipeline");
    g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    struct State {
        QSemaphore                first, resume, endEnqueued;
        std::vector<GstClockTime> delivered;
    } state;
    auto *pad = gst_element_get_static_pad(queue, "sink");
    gst_pad_add_probe(
        pad, GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto &state = *static_cast<State *>(data);
            if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS)
                state.endEnqueued.release();
            return GST_PAD_PROBE_OK;
        },
        &state, nullptr);
    gst_object_unref(pad);
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample          = [](GstAppSink *sink, gpointer data) {
        auto &state  = *static_cast<State *>(data);
        auto *sample = gst_app_sink_pull_sample(sink);
        check(sample, "could not pull raw frame");
        state.delivered.push_back(GST_BUFFER_PTS(gst_sample_get_buffer(sample)));
        if (state.delivered.size() == 1) {
            state.first.release();
            check(state.resume.tryAcquire(1, 5000), "consumer was not resumed");
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &state, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, queue, sink, nullptr);
    check(gst_element_link_many(source, queue, sink, nullptr), "could not link queue regression pipeline");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    for (int i = 0; i < 64; ++i) {
        auto *buffer                = gst_buffer_new_allocate(nullptr, 16, nullptr);
        GST_BUFFER_PTS(buffer)      = guint64(i) * GST_MSECOND;
        GST_BUFFER_DURATION(buffer) = GST_MSECOND;
        check(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK, "could not enqueue raw frame");
        if (i == 0)
            check(state.first.tryAcquire(1, 5000), "first raw frame never reached blocked consumer");
    }
    // EOS is queued behind the last buffer, so the queue has accepted the
    // complete input burst before it can forward EOS to the consumer.
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    check(state.endEnqueued.tryAcquire(1, 5000), "slow consumer blocked the raw producer");
    state.resume.release();
    finish(pipeline);
    if (live) {
        check(state.delivered.size() == 3, "live queue retained stale frames under pressure");
        check(state.delivered[0] == 0 && state.delivered[1] == 62 * GST_MSECOND
                  && state.delivered[2] == 63 * GST_MSECOND,
              "live queue discarded recent frames instead of stale frames");
    } else {
        check(state.delivered.size() == 64, "file input lost raw frames");
        for (unsigned i = 0; i < state.delivered.size(); ++i)
            check(state.delivered[i] == i * GST_MSECOND, "file input changed frame order");
    }
}

guint64 encodeAndDecode(int kbps)
{
    auto *pipeline = gst_pipeline_new(nullptr);
    auto *source   = gst_element_factory_make("videotestsrc", nullptr);
    auto *filter   = gst_element_factory_make("capsfilter", nullptr);
    auto *encoder  = PsiMedia::bins_videoenc_create(QStringLiteral("vp8"), 96, kbps);
    auto *depay    = gst_element_factory_make("rtpvp8depay", nullptr);
    auto *decoder  = gst_element_factory_make("vp8dec", nullptr);
    auto *sink     = gst_element_factory_make("appsink", nullptr);
    check(pipeline && source && filter && encoder && depay && decoder && sink, "could not create VP8 pipeline");
    gst_util_set_object_arg(G_OBJECT(source), "pattern", "smpte");
    g_object_set(source, "num-buffers", 90, nullptr);
    auto *caps = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, 320, "height", G_TYPE_INT, 180, "framerate",
                                     GST_TYPE_FRACTION, 15, 1, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    struct State {
        guint64 bytes  = 0;
        int     frames = 0;
    } state;
    auto *pad = gst_element_get_static_pad(encoder, "src");
    gst_pad_add_probe(
        pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
        [](GstPad *, GstPadProbeInfo *info, gpointer data) {
            auto &state = *static_cast<State *>(data);
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
                auto *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
                for (guint i = 0; i < gst_buffer_list_length(list); ++i)
                    state.bytes += gst_buffer_get_size(gst_buffer_list_get(list, i));
            } else {
                state.bytes += gst_buffer_get_size(GST_PAD_PROBE_INFO_BUFFER(info));
            }
            return GST_PAD_PROBE_OK;
        },
        &state, nullptr);
    gst_object_unref(pad);
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample          = [](GstAppSink *sink, gpointer data) {
        auto &state  = *static_cast<State *>(data);
        auto *sample = gst_app_sink_pull_sample(sink);
        check(sample, "could not pull decoded frame");
        const auto actual   = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
        const auto expected = gst_util_uint64_scale(state.frames, GST_SECOND, 15);
        check(actual == expected, "VP8 encode/decode changed the capture timeline");
        ++state.frames;
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &state, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, filter, encoder, depay, decoder, sink, nullptr);
    check(gst_element_link_many(source, filter, encoder, depay, decoder, sink, nullptr), "could not link VP8 pipeline");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    finish(pipeline);
    check(state.frames == 90, "VP8 lost complete frames without network loss");
    return state.bytes;
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);
    queueUnderPressure(true);
    queueUnderPressure(false);
    const auto low  = encodeAndDecode(100);
    const auto high = encodeAndDecode(1000);
    qInfo("VP8 100kbps=%llu bytes, 1000kbps=%llu bytes", (unsigned long long)low, (unsigned long long)high);
    check(high > low * 2, "requested sending bitrate did not affect encoded output");
    // Saturate the bits/sec conversion rather than overflowing a caller's int.
    auto *encoder = PsiMedia::bins_videoenc_create(QStringLiteral("vp8"), 96, std::numeric_limits<int>::max());
    gst_object_unref(encoder);
    qInfo("Live raw-frame freshness, file losslessness and VP8 bitrate/timestamp regressions passed");
}
