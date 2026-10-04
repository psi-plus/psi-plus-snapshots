// SPDX-License-Identifier: LGPL-2.1-or-later

#include "../gstprovider/bins.h"

#include <QCoreApplication>
#include <QSemaphore>
#include <QSize>
#include <QtGlobal>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <vector>

namespace {

GstElement *findFactory(GstElement *element, const char *factoryName)
{
    if (!GST_IS_BIN(element))
        return nullptr;

    GstIterator *iterator = gst_bin_iterate_elements(GST_BIN(element));
    GValue       item     = G_VALUE_INIT;
    GstElement  *found    = nullptr;
    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        auto              *child   = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory *factory = gst_element_get_factory(child);
        const gchar       *name    = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : nullptr;
        if (name && qstrcmp(name, factoryName) == 0) {
            found = GST_ELEMENT(gst_object_ref(child));
            g_value_reset(&item);
            break;
        }
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(iterator);
    return found;
}

bool hasFactory(GstElement *element, const char *factoryName)
{
    if (!GST_IS_BIN(element))
        return false;

    GstIterator *iterator = gst_bin_iterate_elements(GST_BIN(element));
    GValue       item     = G_VALUE_INIT;
    bool         found    = false;
    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        auto              *child   = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory *factory = gst_element_get_factory(child);
        const gchar       *name    = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : nullptr;
        if (name && qstrcmp(name, factoryName) == 0) {
            found = true;
            g_value_reset(&item);
            break;
        }
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(iterator);
    return found;
}

// Exercise the production graph with timestamps from an already running call.
// Unsynchronised output keeps a 50-second timestamp offset a fast regression.
std::vector<GstClockTime> prepareFrames(bool live, int fps, const std::vector<GstClockTime> &timestamps,
                                        bool encode = false, int inputFps = 120, int inputFpsDen = 1)
{
    auto *pipeline = gst_pipeline_new(nullptr);
    auto *source   = gst_element_factory_make("appsrc", nullptr);
    auto *prep     = PsiMedia::bins_videoprep_create(QSize(32, 24), fps, live);
    auto *sink     = gst_element_factory_make("appsink", nullptr);
    if (!pipeline || !source || !prep || !sink)
        qFatal("Could not construct timestamp regression pipeline");
    auto *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT, 32, "height",
                                     G_TYPE_INT, 24, "framerate", GST_TYPE_FRACTION, inputFps, inputFpsDen, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    g_object_set(source, "format", GST_FORMAT_TIME, "is-live", live, nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    struct Capture {
        bool                      encoded = false;
        std::vector<GstClockTime> timestamps;
        QSemaphore                firstFrame;
    } capture;
    capture.encoded               = encode;
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample          = [](GstAppSink *sink, gpointer data) -> GstFlowReturn {
        auto *sample = gst_app_sink_pull_sample(sink);
        if (!sample)
            return GST_FLOW_ERROR;
        auto &capture       = *static_cast<Capture *>(data);
        auto *buffer        = gst_sample_get_buffer(sample);
        bool  completeFrame = true;
        if (capture.encoded) {
            GstMapInfo rtp;
            if (!gst_buffer_map(buffer, &rtp, GST_MAP_READ) || rtp.size < 12 || (rtp.data[0] >> 6) != 2)
                qFatal("Video encoder produced invalid RTP");
            completeFrame = (rtp.data[1] & 0x80) != 0;
            if ((rtp.data[1] & 0x7f) != 96)
                qFatal("Video encoder lost its negotiated payload type");
            gst_buffer_unmap(buffer, &rtp);
        }
        if (completeFrame) {
            capture.timestamps.push_back(GST_BUFFER_PTS(buffer));
            if (capture.timestamps.size() == 1)
                capture.firstFrame.release();
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &capture, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, prep, sink, nullptr);
    bool linked = false;
    if (encode) {
        auto *encoder = PsiMedia::bins_videoenc_create(QStringLiteral("vp8"), 96, 512);
        if (!encoder)
            qFatal("Could not construct production video encoder");
        gst_bin_add(GST_BIN(pipeline), encoder);
        linked = gst_element_link_many(source, prep, encoder, sink, nullptr);
    } else {
        linked = gst_element_link_many(source, prep, sink, nullptr);
    }
    if (!linked || gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        qFatal("Could not start timestamp regression pipeline");
    bool first = true;
    for (auto pts : timestamps) {
        auto *frame = gst_buffer_new_allocate(nullptr, 32 * 24 * 3, nullptr);
        gst_buffer_memset(frame, 0, 0, gst_buffer_get_size(frame));
        GST_BUFFER_PTS(frame) = pts;
        GST_BUFFER_DURATION(frame)
            = inputFps > 0 ? gst_util_uint64_scale(GST_SECOND, inputFpsDen, inputFps) : GST_CLOCK_TIME_NONE;
        if (gst_app_src_push_buffer(GST_APP_SRC(source), frame) != GST_FLOW_OK)
            qFatal("Could not supply timestamp regression frame");
        // A static desktop may not supply a second frame for a long time.
        // Its first frame must reach the consumer without another frame or EOS.
        if (live && first && !capture.firstFrame.tryAcquire(1, 1000))
            qFatal("Live video retained the first frame while waiting for another frame or EOS");
        first = false;
    }
    gst_app_src_end_of_stream(GST_APP_SRC(source));
    auto *bus = gst_element_get_bus(pipeline);
    auto *message
        = gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool completed = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error   = nullptr;
        gchar  *details = nullptr;
        gst_message_parse_error(message, &error, &details);
        qWarning("Video prep error: %s (%s)", error->message, details ? details : "");
        g_clear_error(&error);
        g_free(details);
    }
    if (message)
        gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL); // joins streaming callbacks before inspecting output
    gst_object_unref(pipeline);
    if (!completed)
        qFatal("Video prep did not complete without waiting for media timestamps");
    return capture.timestamps;
}

void checkLiveTimestamps()
{
    const std::vector<GstClockTime> lateFrames
        = { 50 * GST_SECOND, 50 * GST_SECOND + GST_SECOND / 15, 80 * GST_SECOND };
    const auto output = prepareFrames(true, 15, lateFrames);
    qInfo("Late/sparse live capture: input=%zu output=%zu first-pts-ms=%llu", lateFrames.size(), output.size(),
          output.empty() ? 0ULL : static_cast<unsigned long long>(output.front() / GST_MSECOND));
    if (output != lateFrames)
        qFatal("Live video invented historical frames or changed capture timestamps");

    const auto encoded = prepareFrames(true, 15, lateFrames, true);
    if (encoded != lateFrames)
        qFatal("Production VP8/RTP encoding lost live capture timestamps or introduced a backlog");

    std::vector<GstClockTime> fastFrames;
    for (int frame = 0; frame < 120; ++frame)
        fastFrames.push_back(50 * GST_SECOND + gst_util_uint64_scale(frame, GST_SECOND, 120));
    const auto limited = prepareFrames(true, 15, fastFrames);
    if (limited.empty() || limited.size() > 16)
        qFatal("Live video did not limit the requested frame rate");
    for (auto pts : limited) {
        if (std::find(fastFrames.begin(), fastFrames.end(), pts) == fastFrames.end())
            qFatal("Live rate limiting replaced a capture timestamp");
    }

    const std::vector<GstClockTime> slowFrames = { 50 * GST_SECOND, 50 * GST_SECOND + GST_SECOND / 5 };
    if (prepareFrames(true, 15, slowFrames, false, 5) != slowFrames)
        qFatal("Live capture slower than the requested FPS did not preserve source timing");

    const std::vector<GstClockTime> verySlowFrames = { 50 * GST_SECOND, 52 * GST_SECOND };
    if (prepareFrames(true, 15, verySlowFrames, false, 1, 2) != verySlowFrames)
        qFatal("Sub-one-FPS live capture did not preserve source timing");

    const auto variable = prepareFrames(true, 15, fastFrames, false, 0);
    if (variable.empty() || variable.size() > 16)
        qFatal("Variable-rate capture exceeded the requested live FPS ceiling");
    if (prepareFrames(true, 15, lateFrames, true, 0) != lateFrames)
        qFatal("Variable-rate VP8/RTP video did not preserve capture timestamps");

    const auto fileFrames = prepareFrames(false, 30, { 0, GST_SECOND / 5 });
    if (fileFrames.size() <= 2 || fileFrames.front() != 0)
        qFatal("File playback lost its constant-cadence frame duplication");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    gst_init(nullptr, nullptr);

    GstElement *nativeCadence = PsiMedia::bins_videoprep_create(QSize(640, 480), -1, true);
    if (!nativeCadence)
        qFatal("Could not construct native-cadence video prep");
    if (hasFactory(nativeCadence, "videorate"))
        qFatal("Unspecified live FPS unexpectedly inserts videorate");

    GstElement *scale = findFactory(nativeCadence, "videoscale");
    if (!scale)
        qFatal("Video prep did not contain videoscale");
    gboolean addBorders = FALSE;
    g_object_get(G_OBJECT(scale), "add-borders", &addBorders, nullptr);
    gst_object_unref(scale);
    if (!addBorders)
        qFatal("Video prep may distort source display aspect ratio");

    GstElement *filter = findFactory(nativeCadence, "capsfilter");
    if (!filter)
        qFatal("Video prep did not contain scale capsfilter");
    GstCaps *filterCaps = nullptr;
    g_object_get(G_OBJECT(filter), "caps", &filterCaps, nullptr);
    gst_object_unref(filter);
    if (!filterCaps || gst_caps_get_size(filterCaps) != 1)
        qFatal("Video prep scale caps are missing");
    const GstStructure *filterStructure = gst_caps_get_structure(filterCaps, 0);
    int                 parNum          = 0;
    int                 parDen          = 0;
    const bool          squarePar = gst_structure_get_fraction(filterStructure, "pixel-aspect-ratio", &parNum, &parDen)
        && parNum == 1 && parDen == 1;
    gst_caps_unref(filterCaps);
    if (!squarePar)
        qFatal("Video prep does not force square pixel aspect ratio");

    gst_object_unref(nativeCadence);

    GstElement *forcedCadence = PsiMedia::bins_videoprep_create(QSize(640, 480), 30, true);
    if (!forcedCadence)
        qFatal("Could not construct explicit-cadence video prep");
    if (!hasFactory(forcedCadence, "videorate"))
        qFatal("Explicit FPS did not insert videorate");
    gst_object_unref(forcedCadence);

    checkLiveTimestamps();
    qInfo("Live video cadence regression passed");
    return 0;
}
