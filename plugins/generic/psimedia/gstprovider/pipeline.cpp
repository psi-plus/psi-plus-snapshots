/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "pipeline.h"

#include "devices.h"

#include <QList>
#include <QSet>
#include <gst/gst.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

// FIXME: this file is heavily commented out and a mess, mainly because
//   all of my attempts at a dynamic pipeline were futile.  someday we
//   can uncomment and clean this up...

#define PIPELINE_DEBUG

// rates lower than 22050 (e.g. 16000) might not work with echo-cancel
#define DEFAULT_FIXED_RATE 22050

// in milliseconds
#define DEFAULT_LATENCY 20

#define WEBRTCDSP_RATE 48000

namespace PsiMedia {

static int get_fixed_rate()
{
    QString val = QString::fromLatin1(qgetenv("PSI_FIXED_RATE"));
    if (!val.isEmpty()) {
        int rate = val.toInt();
        if (rate > 0)
            return rate;
        else
            return 0;
    } else
        return DEFAULT_FIXED_RATE;
}

static int get_latency_time()
{
    QString val = QString::fromLatin1(qgetenv("PSI_AUDIO_LTIME"));
    if (!val.isEmpty()) {
        int x = val.toInt();
        if (x > 0)
            return x;
        else
            return 0;
    } else
        return DEFAULT_LATENCY;
}

static const char *type_to_str(PDevice::Type type)
{
    switch (type) {
    case PDevice::AudioIn:
        return "AudioIn";
    case PDevice::AudioOut:
        return "AudioOut";
    case PDevice::VideoIn:
        return "VideoIn";
    default:
        Q_ASSERT(0);
        return nullptr;
    }
}

static void videosrcbin_pad_added(GstElement *element, GstPad *pad, gpointer data)
{
    Q_UNUSED(element);
    auto gpad = static_cast<GstPad *>(data);

    // gchar *name = gst_pad_get_name(pad);
    // qDebug("videosrcbin pad-added: %s", name);
    // g_free(name);

    // GstCaps *caps = gst_pad_get_caps(pad);
    // gchar *gstr = gst_caps_to_string(caps);
    // QString capsString = QString::fromUtf8(gstr);
    // g_free(gstr);
    // qDebug("  caps: [%s]", qPrintable(capsString));

    gst_ghost_pad_set_target(GST_GHOST_PAD(gpad), pad);

    // gst_caps_unref(caps);
}

static GstStaticPadTemplate videosrcbin_template
    = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw"));

static GstCaps *filter_for_capture_size(const QSize &size)
{
    return gst_caps_new_full(gst_structure_new("video/x-raw", "width", G_TYPE_INT, size.width(), "height", G_TYPE_INT,
                                               size.height(), nullptr),
                             gst_structure_new("image/jpeg", "width", G_TYPE_INT, size.width(), "height", G_TYPE_INT,
                                               size.height(), nullptr),
                             gst_structure_new("video/x-h264", "width", G_TYPE_INT, size.width(), "height", G_TYPE_INT,
                                               size.height(), nullptr),
                             nullptr);
}

static int videoMimeRank(const QString &mime)
{
    if (mime == QLatin1String("video/x-raw"))
        return 0;
    if (mime == QLatin1String("image/jpeg"))
        return 1;
    if (mime == QLatin1String("video/x-h264"))
        return 2;
    return -1;
}

static double videoCaptureScore(const QSize &desiredSize, int preferredFps, const QString &mime, int width, int height,
                                int fpsNumerator, int fpsDenominator)
{
    if (width <= 0 || height <= 0 || fpsNumerator <= 0 || fpsDenominator <= 0)
        return std::numeric_limits<double>::infinity();

    const double desiredAspect = double(desiredSize.width()) / double(desiredSize.height());
    const double actualAspect  = double(width) / double(height);
    const double aspectError   = std::abs(actualAspect / desiredAspect - 1.0);
    const double widthError    = std::abs(double(width - desiredSize.width())) / double(desiredSize.width());
    const double heightError   = std::abs(double(height - desiredSize.height())) / double(desiredSize.height());
    const double fps           = double(fpsNumerator) / double(fpsDenominator);
    const double fpsError      = std::abs(fps - preferredFps) / double(preferredFps);
    const int    mimeRank      = videoMimeRank(mime);

    if (mimeRank < 0)
        return std::numeric_limits<double>::infinity();

    // Capture cadence is the strongest quality constraint. Geometry is
    // secondary because videoprep preserves display aspect ratio with borders,
    // so a smooth nearby mode is better than an exact 5 fps mode. MIME is only
    // a tie breaker, preferring raw to avoid an unnecessary decode.
    return fpsError * 100.0 + aspectError * 20.0 + (widthError + heightError) * 10.0 + mimeRank * 0.01;
}

static GstCaps *filter_for_desired_size(GstDevice *dev, const QSize &size, int preferredFps, QString *selectedMime)
{
    if (selectedMime)
        selectedMime->clear();
    preferredFps = preferredFps > 0 ? preferredFps : 30;

    GstCaps *bestCaps  = nullptr;
    double   bestScore = std::numeric_limits<double>::infinity();
    QString  bestMime;
    int      bestWidth  = 0;
    int      bestHeight = 0;
    int      bestFpsNum = 0;
    int      bestFpsDen = 1;

    // Prefer the complete GstDevice caps. Unlike PDevice::Caps these preserve
    // PipeWire/V4L2 lists and ranges, which are common for webcam modes.
    GstCaps *nativeCaps
        = dev->nativeCaps.isEmpty() ? nullptr : gst_caps_from_string(dev->nativeCaps.toUtf8().constData());
    if (nativeCaps && !gst_caps_is_empty(nativeCaps) && !gst_caps_is_any(nativeCaps)) {
        for (guint i = 0; i < gst_caps_get_size(nativeCaps); ++i) {
            const GstStructure *source = gst_caps_get_structure(nativeCaps, i);
            const QString       mime   = QString::fromLatin1(gst_structure_get_name(source));
            if (videoMimeRank(mime) < 0)
                continue;

            GstStructure *fixed = gst_structure_copy(source);
            if (gst_structure_has_field(fixed, "width"))
                gst_structure_fixate_field_nearest_int(fixed, "width", size.width());
            else
                gst_structure_set(fixed, "width", G_TYPE_INT, size.width(), nullptr);

            if (gst_structure_has_field(fixed, "height"))
                gst_structure_fixate_field_nearest_int(fixed, "height", size.height());
            else
                gst_structure_set(fixed, "height", G_TYPE_INT, size.height(), nullptr);

            if (gst_structure_has_field(fixed, "framerate"))
                gst_structure_fixate_field_nearest_fraction(fixed, "framerate", preferredFps, 1);
            else
                gst_structure_set(fixed, "framerate", GST_TYPE_FRACTION, preferredFps, 1, nullptr);

            gst_structure_fixate(fixed);

            int width  = 0;
            int height = 0;
            int fpsNum = 0;
            int fpsDen = 1;
            if (!gst_structure_get_int(fixed, "width", &width) || !gst_structure_get_int(fixed, "height", &height)
                || !gst_structure_get_fraction(fixed, "framerate", &fpsNum, &fpsDen)) {
                gst_structure_free(fixed);
                continue;
            }

            const double score = videoCaptureScore(size, preferredFps, mime, width, height, fpsNum, fpsDen);
            if (score >= bestScore) {
                gst_structure_free(fixed);
                continue;
            }

            // GstDevice caps describe device capabilities, not necessarily a
            // filter that is safe to push back into the created source element.
            // PipeWire in particular may advertise memory features (for example
            // DMABuf) and extra format fields that are valid for discovery but
            // make a dynamically created pipewiresrc fail to negotiate when
            // copied verbatim. Use the full caps only for mode selection, then
            // rebuild a minimal fixed filter in normal system memory.
            GstCaps *candidate
                = gst_caps_new_simple(mime.toLatin1().constData(), "width", G_TYPE_INT, width, "height", G_TYPE_INT,
                                      height, "framerate", GST_TYPE_FRACTION, fpsNum, fpsDen, nullptr);
            gst_structure_free(fixed);

            if (bestCaps)
                gst_caps_unref(bestCaps);
            bestCaps   = candidate;
            bestScore  = score;
            bestMime   = mime;
            bestWidth  = width;
            bestHeight = height;
            bestFpsNum = fpsNum;
            bestFpsDen = fpsDen;
        }
    }
    if (nativeCaps)
        gst_caps_unref(nativeCaps);

    // Compatibility fallback for platform monitors that only expose the legacy
    // fixed PDevice::Caps list.
    if (!bestCaps) {
        for (const auto &candidate : dev->caps) {
            const double score
                = videoCaptureScore(size, preferredFps, candidate.mime, candidate.video.width, candidate.video.height,
                                    candidate.video.framerate_numerator, candidate.video.framerate_denominator);
            if (score >= bestScore)
                continue;

            if (bestCaps)
                gst_caps_unref(bestCaps);
            bestCaps   = gst_caps_new_simple(candidate.mime.toLatin1().constData(), "width", G_TYPE_INT,
                                             candidate.video.width, "height", G_TYPE_INT, candidate.video.height,
                                             "framerate", GST_TYPE_FRACTION, candidate.video.framerate_numerator,
                                             candidate.video.framerate_denominator, nullptr);
            bestScore  = score;
            bestMime   = candidate.mime;
            bestWidth  = candidate.video.width;
            bestHeight = candidate.video.height;
            bestFpsNum = candidate.video.framerate_numerator;
            bestFpsDen = candidate.video.framerate_denominator;
        }
    }

    if (!bestCaps) {
        // Last-resort source negotiation. Keep it raw, but still request the
        // intended call geometry/cadence rather than accepting an arbitrary
        // low-fps default from PipeWire.
        bestMime   = QStringLiteral("video/x-raw");
        bestWidth  = size.width();
        bestHeight = size.height();
        bestFpsNum = preferredFps;
        bestFpsDen = 1;
        bestCaps = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, bestWidth, "height", G_TYPE_INT, bestHeight,
                                       "framerate", GST_TYPE_FRACTION, bestFpsNum, bestFpsDen, nullptr);
    }

    if (selectedMime)
        *selectedMime = bestMime;
#ifdef PIPELINE_DEBUG
    qDebug("VideoIn selected capture mode=%s %dx%d @ %d/%d", qPrintable(bestMime), bestWidth, bestHeight, bestFpsNum,
           bestFpsDen);
#endif
    return bestCaps;
}

GstCaps *selectVideoCaptureCaps(const QString &nativeCaps, const QSize &desiredSize, int preferredFps,
                                QString *selectedMime)
{
    GstDevice device;
    device.nativeCaps = nativeCaps;
    return filter_for_desired_size(&device, desiredSize, preferredFps, selectedMime);
}

static GstElement *make_webrtcdsp_filter()
{
    GstStructure *cs;
    GstCaps      *caps = gst_caps_new_empty();
    cs = gst_structure_new("audio/x-raw", "rate", G_TYPE_INT, WEBRTCDSP_RATE, "format", G_TYPE_STRING, "S16LE",
                           "channels", G_TYPE_INT, 1, "channel-mask", GST_TYPE_BITMASK, 1, nullptr);
    gst_caps_append_structure(caps, cs);
    GstElement *capsfilter = gst_element_factory_make("capsfilter", nullptr);
    g_object_set(G_OBJECT(capsfilter), "caps", caps, nullptr);
    gst_caps_unref(caps);
    return capsfilter;
}

//----------------------------------------------------------------------------
// PipelineContext
//----------------------------------------------------------------------------
class PipelineDevice;

class PipelineDeviceContextPrivate {
public:
    PipelineContext      *pipeline = nullptr;
    PipelineDevice       *device   = nullptr;
    PipelineDeviceOptions opts;
    bool                  activated = false;

    // queue for srcs, adder for sinks
    GstElement *element = nullptr;
};

class PipelineDevice {
public:
    int           refs = 0;
    QString       id;
    PDevice::Type type;
    GstElement   *pipeline   = nullptr;
    GstElement   *device_bin = nullptr;
    bool          activated  = false;
    QString       webrtcEchoProbeName; // initialized when we modify already running AudioIn dev

    QSet<PipelineDeviceContextPrivate *> contexts;

    // for srcs
    GstElement *tee                  = nullptr;
    GstElement *aindev               = nullptr;
    bool        webrtcdspInitialized = false;

    // for sinks (audio only, video sinks are always unshared)
    GstElement *audioconvert  = nullptr;
    GstElement *audioresample = nullptr;
    GstElement *webrtcprobe   = nullptr;

private:
    GstElement *makeDeviceBin(const PipelineDeviceOptions &options, DeviceMonitor *deviceMonitor)
    {
        QSize       captureSize;
        GstElement *deviceElement = devices_makeElement(id, type, &captureSize);
        if (!deviceElement)
            return nullptr;

        // explicitly set audio devices to be low-latency
        if (/*type == PDevice::AudioIn ||*/ type == PDevice::AudioOut) {
            auto name = gst_element_get_name(deviceElement);
            if (QLatin1String { name }.contains(QLatin1String { "alsa" })) {
                int latency_ms = get_latency_time();
                if (latency_ms > 0) {
                    gint64 lt = latency_ms * 1000; // microseconds
                    g_object_set(G_OBJECT(deviceElement), "latency-time", lt, nullptr);
                    // g_object_set(G_OBJECT(e), "buffer-time", 2 * lt, nullptr);
                }
            }
        }

        GstElement *bin = gst_bin_new(nullptr); // FIXME not necessary for audio?

        if (type == PDevice::AudioIn) {
            aindev = deviceElement;
            GstPad *pad;
            gst_element_set_name(deviceElement, "aindev");
            gst_bin_add(GST_BIN(bin), deviceElement);

            bool aecAvailable = true;
            if (options.aec) {
                GstElementFactory *factory = gst_element_factory_find("webrtcdsp");
                aecAvailable               = factory != nullptr;
                if (factory)
                    gst_object_unref(factory);
                else
                    qWarning("Failed to find GStreamer webrtcdsp element. Echo cancellation was disabled");
            }

            if (options.aec && aecAvailable) {

                GstElement *audioconvert  = gst_element_factory_make("audioconvert", nullptr);
                GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
                GstElement *capsfilter    = make_webrtcdsp_filter();
                GstElement *webrtcdsp     = gst_element_factory_make("webrtcdsp", nullptr);
                g_object_set(webrtcdsp, "probe", options.echoProberName.toLatin1().constData(), nullptr);

                gst_bin_add(GST_BIN(bin), audioconvert);
                gst_bin_add(GST_BIN(bin), audioresample);
                gst_bin_add(GST_BIN(bin), capsfilter);
                gst_bin_add(GST_BIN(bin), webrtcdsp);

                gst_element_link_many(deviceElement, audioconvert, audioresample, capsfilter, webrtcdsp, nullptr);
                pad = gst_element_get_static_pad(webrtcdsp, "src");

                webrtcdspInitialized = true;
            } else {
                pad = gst_element_get_static_pad(deviceElement, "src");
            }
            gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
            gst_object_unref(GST_OBJECT(pad));
        } else if (type == PDevice::VideoIn) {

            auto device = deviceMonitor ? deviceMonitor->device(id) : nullptr;
            if (!device) {
                // Synthetic/custom GStreamer sources (for example videotestsrc)
                // do not have hardware DeviceMonitor metadata. Accept a raw-video
                // source directly; real enumerated cameras keep the caps/decode
                // selection path below.
                GstPad *pad = gst_element_get_static_pad(deviceElement, "src");
                if (!pad) {
                    gst_object_unref(deviceElement);
                    return nullptr;
                }
                gst_bin_add(GST_BIN(bin), deviceElement);
                gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
                gst_object_unref(pad);
                return bin;
            }

#ifdef Q_OS_MAC
            // FIXME: hardcode resolution because filter_for_desired_size
            //   doesn't really work with osxvideosrc due to the fact that
            //   it can handle any resolution.  for example, setting
            //   desiredSize to 320x240 yields a caps of 320x480 which is
            //   wrong (and may crash videoscale, but that's another
            //   matter).  We'll hardcode the caps to 320x240, since that's
            //   the resolution psimedia currently wants anyway,
            //   as opposed to not specifying a captureSize, which would
            //   also work fine but may result in double-resizing.
            captureSize = QSize(640, 480);
#endif
            // return e; // fixme review if we need all the below. it seems it forces double conversion
            // (yuy2 -> Y42B for rtp and yuy2 for preview. while w/o it we have i420 on input and conert only for
            // preview)

            /* TODO we need approach similar to
gst-launch-1.0 -v autovideosrc ! switchbin num-paths=3 \
path0::caps="video/x-h264" path0::element="h264parse ! avdec_h264" \
path1::caps="image/jpeg"   path1::element="jpegdec" \
path2::caps="video/x-raw" \
! videoconvert ! autovideosink
*/

            GstCaps *capsfilter = nullptr;
            QString  selectedMime;
            if (captureSize.isValid()) {
                capsfilter   = filter_for_capture_size(captureSize);
                selectedMime = QStringLiteral("video/x-raw");
            } else if (options.videoSize.isValid()) {
                // PipeWire devices frequently expose only range-valued caps.
                // The legacy fixed PDevice::Caps view is then empty. Feeding a
                // fixated discovery mode back into pipewiresrc proved brittle
                // with real cameras: the source stayed PLAYING with no current
                // caps or buffers. Preserve the previously working behavior for
                // that case and let PipeWire choose a concrete raw mode; the
                // downstream video-prep chain still scales it to the call size.
                if (id.startsWith(QLatin1String("pipewiresrc")) && device->caps.isEmpty()
                    && device->nativeCaps.contains(QLatin1String("video/x-raw"))) {
                    capsfilter   = gst_caps_new_empty_simple("video/x-raw");
                    selectedMime = QStringLiteral("video/x-raw");
#ifdef PIPELINE_DEBUG
                    qDebug("VideoIn PipeWire range-only caps: leaving source mode unfixed");
#endif
                } else {
                    capsfilter = filter_for_desired_size(device, options.videoSize, options.fps > 0 ? options.fps : 30,
                                                         &selectedMime);
                }
            }

            if (selectedMime.isEmpty()) {
                if (device->caps.isEmpty()) {
                    // DeviceMonitor only records fixed tuples. PipeWire/V4L2
                    // cameras that advertise ranges can therefore legitimately
                    // arrive here with no fixed entries at all.
                    selectedMime = QStringLiteral("video/x-raw");
                } else {
                    const auto hasMime = [device](const QString &mime) {
                        return std::any_of(device->caps.begin(), device->caps.end(),
                                           [&mime](const auto &c) { return c.mime == mime; });
                    };
                    if (hasMime(QStringLiteral("video/x-raw")))
                        selectedMime = QStringLiteral("video/x-raw");
                    else if (hasMime(QStringLiteral("image/jpeg")))
                        selectedMime = QStringLiteral("image/jpeg");
                    else if (hasMime(QStringLiteral("video/x-h264")))
                        selectedMime = QStringLiteral("video/x-h264");
                }
            }

            const auto failVideoBin = [&]() -> GstElement * {
                if (capsfilter) {
                    gst_caps_unref(capsfilter);
                    capsfilter = nullptr;
                }
                gst_object_unref(bin);
                return nullptr;
            };
            const auto addVideoElement = [bin](GstElement *element) {
                if (!element)
                    return false;
                if (gst_bin_add(GST_BIN(bin), element))
                    return true;
                gst_object_unref(element);
                return false;
            };

            if (!gst_bin_add(GST_BIN(bin), deviceElement)) {
                gst_object_unref(deviceElement);
                return failVideoBin();
            }

            GstPadTemplate *srcTemplate = gst_static_pad_template_get(&videosrcbin_template);
            GstPad *binPad = srcTemplate ? gst_ghost_pad_new_no_target_from_template("src", srcTemplate) : nullptr;
            if (srcTemplate)
                gst_object_unref(srcTemplate);
            if (!binPad || !gst_element_add_pad(bin, binPad)) {
                if (binPad)
                    gst_object_unref(binPad);
                return failVideoBin();
            }

            const auto setGhostTarget = [binPad](GstElement *element) {
                GstPad *srcPad = element ? gst_element_get_static_pad(element, "src") : nullptr;
                if (!srcPad)
                    return false;
                const bool ok = gst_ghost_pad_set_target(GST_GHOST_PAD(binPad), srcPad);
                gst_object_unref(srcPad);
                return ok;
            };

            QList<GstElement *> toLink;
            // Decoder topology must match the caps we selected above, not any
            // format the camera happens to advertise as an alternative.
            if (selectedMime == QLatin1String("video/x-raw")) {
                GstElement *videoconvert = gst_element_factory_make("videoconvert", nullptr);
                if (!addVideoElement(videoconvert))
                    return failVideoBin();
                toLink.append(videoconvert);
                if (!setGhostTarget(videoconvert))
                    return failVideoBin();
            } else if (selectedMime == QLatin1String("image/jpeg")) {
                GstElement *jpegdec = gst_element_factory_make("jpegdec", nullptr);
                if (!addVideoElement(jpegdec))
                    return failVideoBin();
                toLink.append(jpegdec);
                if (!setGhostTarget(jpegdec))
                    return failVideoBin();
            } else if (selectedMime == QLatin1String("video/x-h264")) {
                GstElement *h264parse  = gst_element_factory_make("h264parse", nullptr);
                GstElement *avdec_h264 = gst_element_factory_make("avdec_h264", nullptr);
                if (!addVideoElement(h264parse) || !addVideoElement(avdec_h264))
                    return failVideoBin();
                toLink.append(h264parse);
                toLink.append(avdec_h264);
                if (!setGhostTarget(avdec_h264))
                    return failVideoBin();
            } else {
                GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
                if (!addVideoElement(decodebin))
                    return failVideoBin();
                toLink.append(decodebin);

                g_signal_connect(G_OBJECT(decodebin), "pad-added", G_CALLBACK(videosrcbin_pad_added), binPad);
            }

            //             GstElement *switchbin = gst_parse_launch(R"GST(switchbin num-paths=3
            // path0::caps="video/x-h264" path0::element="h264parse ! avdec_h264"
            // path1::caps="image/jpeg"   path1::element="jpegdec"
            // path2::caps="video/x-raw")GST",
            //                                                      nullptr);
            //             gst_bin_add(GST_BIN(bin), switchbin);

            bool sourceLinked = false;
            if (capsfilter) {
                sourceLinked = gst_element_link_filtered(deviceElement, toLink[0], capsfilter);
                gst_caps_unref(capsfilter);
                capsfilter = nullptr;
            } else {
                sourceLinked = gst_element_link(deviceElement, toLink[0]);
            }
            if (!sourceLinked)
                return failVideoBin();

            for (int i = 1; i < toLink.size(); ++i) {
                if (!gst_element_link(toLink[i - 1], toLink[i]))
                    return failVideoBin();
            }
        } else // AudioOut
        {
            GstElement *audioconvert  = gst_element_factory_make("audioconvert", nullptr);
            GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);

            gchar *name_value = nullptr;
            webrtcprobe       = gst_element_factory_make("webrtcechoprobe", nullptr);
            if (webrtcprobe) {
                g_object_get(G_OBJECT(webrtcprobe), "name", &name_value, nullptr);
                webrtcEchoProbeName = QString::fromLatin1(name_value);
                g_free(name_value);
            } else {
                qWarning("Failed to create GStreamer webrtcechoprobe element instance. Echo cancellation was disabled");
            }

            GstElement *capsfilter = nullptr;
            gst_bin_add(GST_BIN(bin), audioconvert);
            gst_bin_add(GST_BIN(bin), audioresample);
            if (webrtcprobe) {
                // build resampler caps
                GstStructure *cs;
                GstCaps      *caps = gst_caps_new_empty();
                cs = gst_structure_new("audio/x-raw", "rate", G_TYPE_INT, WEBRTCDSP_RATE, "format", G_TYPE_STRING,
                                       "S16LE", "channels", G_TYPE_INT, 2, "channel-mask", GST_TYPE_BITMASK, 3,
                                       nullptr);
                gst_caps_append_structure(caps, cs);
                capsfilter = gst_element_factory_make("capsfilter", nullptr);
                g_object_set(G_OBJECT(capsfilter), "caps", caps, nullptr);
                gst_caps_unref(caps);

                gst_bin_add(GST_BIN(bin), capsfilter);
                gst_bin_add(GST_BIN(bin), webrtcprobe);
            }
            gst_bin_add(GST_BIN(bin), deviceElement);

            if (webrtcprobe)
                gst_element_link_many(audioconvert, audioresample, capsfilter, webrtcprobe, deviceElement, nullptr);
            else
                gst_element_link_many(audioconvert, audioresample, deviceElement, nullptr);

            GstPad *pad = gst_element_get_static_pad(audioconvert, "sink");
            gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
            gst_object_unref(GST_OBJECT(pad));
        }

        return bin;
    }

public:
    PipelineDevice(const QString &_id, PDevice::Type _type, PipelineDeviceContextPrivate *context,
                   DeviceMonitor *deviceMonitor) : refs(0), id(_id), type(_type)
    {
        pipeline = context->pipeline->element();

        device_bin = makeDeviceBin(context->opts, deviceMonitor);
        if (!device_bin) {
            qWarning("Failed to create device");
            return;
        }

        // TODO: use context->opts.fps?

        if (type == PDevice::AudioIn || type == PDevice::VideoIn) {
            tee = gst_element_factory_make("tee", nullptr);
            // gst_element_set_locked_state(tee, TRUE);
            gst_bin_add(GST_BIN(pipeline), tee);

            // gst_element_set_locked_state(bin, TRUE);
            gst_bin_add(GST_BIN(pipeline), device_bin);
            gst_element_link(device_bin, tee);
        } else // AudioOut
        {
            gst_bin_add(GST_BIN(pipeline), device_bin);

            // sink starts out activated
            activated = true;
        }

        addRef(context);
    }

    ~PipelineDevice()
    {
        Q_ASSERT(contexts.isEmpty());

        if (!device_bin)
            return;

        if (type == PDevice::AudioIn || type == PDevice::VideoIn) {
            gst_bin_remove(GST_BIN(pipeline), device_bin);

            if (tee)
                gst_bin_remove(GST_BIN(pipeline), tee);
        } else // AudioOut
        {
            gst_element_set_state(device_bin, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(pipeline), device_bin);
        }
    }

    void addRef(PipelineDeviceContextPrivate *context)
    {
        Q_ASSERT(!contexts.contains(context));

        // TODO: consider context->opts for refs after first

        if (type == PDevice::AudioIn || type == PDevice::VideoIn) {
            // create a queue from the tee, and hand it off.  app
            //   uses this queue element as if it were the actual
            //   device
            GstElement *queue
                = gst_element_factory_make("queue", type == PDevice::AudioIn ? "queue_audioin" : "queue_videoin");
            context->element = queue;
            // gst_element_set_locked_state(queue, TRUE);
            gst_bin_add(GST_BIN(pipeline), queue);
            gst_element_link(tee, queue);
        } else // AudioOut
        {
            context->element = device_bin;
            // sink starts out activated
            context->activated = true;
        }

        contexts += context;
        ++refs;
    }

    void removeRef(PipelineDeviceContextPrivate *context)
    {
        Q_ASSERT(contexts.contains(context));

        // TODO: recalc video properties

        if (type == PDevice::AudioIn || type == PDevice::VideoIn) {
            // deactivate if not done so already
            deactivate(context);

            GstElement *queue = context->element;
            gst_bin_remove(GST_BIN(pipeline), queue);
        }

        contexts.remove(context);
        --refs;
    }

    void activate(PipelineDeviceContextPrivate *context)
    {
        // activate the context
        if (!context->activated) {
            // GstElement *queue = context->element;
            // gst_element_set_locked_state(queue, FALSE);
            // gst_element_set_state(queue, GST_STATE_PLAYING);
            context->activated = true;
        }

        // activate the device
        if (!activated) {
            // gst_element_set_locked_state(tee, FALSE);
            // gst_element_set_locked_state(bin, FALSE);
            // gst_element_set_state(tee, GST_STATE_PLAYING);
            // gst_element_set_state(bin, GST_STATE_PLAYING);
            activated = true;
        }
    }

    void deactivate(PipelineDeviceContextPrivate *context)
    {
#if 0
        if(activated && refs == 1)
        {
            if(type == PDevice::AudioIn || type == PDevice::VideoIn)
            {
                gst_element_set_locked_state(bin, TRUE);

                if(speexdsp)
                    gst_element_set_locked_state(speexdsp, TRUE);

                if(tee)
                    gst_element_set_locked_state(tee, TRUE);
            }
        }

        if(context->activated)
        {
            if(type == PDevice::AudioIn || type == PDevice::VideoIn)
            {
                GstElement *queue = context->element;
                gst_element_set_locked_state(queue, TRUE);
            }
        }

        if(activated && refs == 1)
        {
            if(type == PDevice::AudioIn || type == PDevice::VideoIn)
            {
                gst_element_set_state(bin, GST_STATE_NULL);
                gst_element_get_state(bin, nullptr, nullptr, GST_CLOCK_TIME_NONE);

                //qDebug("set to null");
                if(speexdsp)
                {
                    gst_element_set_state(speexdsp, GST_STATE_NULL);
                    gst_element_get_state(speexdsp, nullptr, nullptr, GST_CLOCK_TIME_NONE);
                }
NULL
                if(tee)
                {
                    gst_element_set_state(tee, GST_STATE_NULL);
                    gst_element_get_state(tee, nullptr, nullptr, GST_CLOCK_TIME_NONE);
                }
            }
        }

        if(context->activated)
        {
            if(type == PDevice::AudioIn || type == PDevice::VideoIn)
            {
                GstElement *queue = context->element;

                // FIXME: until we fix this, we only support 1 ref
                // get tee and prepare srcpad
                /*GstPad *sinkpad = gst_element_get_pad(queue, "sink");
                GstPad *srcpad = gst_pad_get_peer(sinkpad);
                gst_object_unref(GST_OBJECT(sinkpad));
                gst_element_release_request_pad(tee, srcpad);
                gst_object_unref(GST_OBJECT(srcpad));*/

                // set queue to null state
                gst_element_set_state(queue, GST_STATE_NULL);
                gst_element_get_state(queue, nullptr, nullptr, GST_CLOCK_TIME_NONE);

                context->activated = false;
            }
        }

        if(activated && refs == 1)
        {
            if(type == PDevice::AudioIn || type == PDevice::VideoIn)
                activated = false;
        }
#endif
        // FIXME
        context->activated = false;
        activated          = false;
    }

    void update(const PipelineDeviceContext &ctx)
    {
        // TODO: change video properties based on options
        if (type == PDevice::AudioIn && ctx.options().aec && !webrtcdspInitialized) {
            // seems like we want to enable AEC. for this we have to modify already running pipeline
            if (!aindev) {
                qWarning("AudioIn device is not found. failed to insert DSP element");
                return;
            }
            webrtcEchoProbeName  = ctx.options().echoProberName;
            webrtcdspInitialized = true; // just prevent conequent calls to this function

            struct F {
                static GstPadProbeReturn cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
                {
                    auto pipeline = reinterpret_cast<PipelineDevice *>(user_data);
                    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

                    // insert webrtcdsp after audio input device
                    // useful article:
                    // https://gstreamer.freedesktop.org/documentation/application-development/advanced/pipeline-manipulation.html?gi-language=c#dynamically-changing-the-pipeline

                    GstElement *audioconvert  = gst_element_factory_make("audioconvert", nullptr);
                    GstElement *audioresample = gst_element_factory_make("audioresample", nullptr);
                    GstElement *capsfilter    = make_webrtcdsp_filter();
                    GstElement *webrtcdsp     = gst_element_factory_make("webrtcdsp", nullptr);
                    g_object_set(webrtcdsp, "probe", pipeline->webrtcEchoProbeName.toLatin1().constData(), nullptr);

                    gst_bin_add(GST_BIN(pipeline->device_bin), audioconvert);
                    gst_bin_add(GST_BIN(pipeline->device_bin), audioresample);
                    gst_bin_add(GST_BIN(pipeline->device_bin), capsfilter);
                    gst_bin_add(GST_BIN(pipeline->device_bin), webrtcdsp);

                    pad            = gst_element_get_static_pad(webrtcdsp, "src");
                    GstPad *binPad = gst_element_get_static_pad(pipeline->device_bin, "src");
                    gst_ghost_pad_set_target((GstGhostPad *)binPad, pad);
                    g_object_unref(G_OBJECT(binPad));
                    gst_element_link_many(pipeline->aindev, audioconvert, audioresample, capsfilter, webrtcdsp,
                                          nullptr);

                    gst_element_sync_state_with_parent(audioconvert);
                    gst_element_sync_state_with_parent(audioresample);
                    gst_element_sync_state_with_parent(capsfilter);
                    gst_element_sync_state_with_parent(webrtcdsp);

                    return GST_PAD_PROBE_REMOVE;
                }
            };
            GstPad *blockpad = gst_element_get_static_pad(aindev, "src");
            gst_pad_add_probe(blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, &F::cb, this, nullptr);
        }
    }

    QString echoProbeName() const { return webrtcEchoProbeName; }
};

class PipelineContext::Private {
public:
    GstElement            *pipeline;
    bool                   activated;
    QSet<PipelineDevice *> devices;

    Private() : activated(false) { pipeline = gst_pipeline_new(nullptr); }

    ~Private()
    {
        Q_ASSERT(devices.isEmpty());
        deactivate();
        g_object_unref(G_OBJECT(pipeline));
    }

    void activate()
    {
        if (!activated) {
            GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
            // qDebug("gst_element_set_state pipline GST_STATE_PLAYING => %d", ret);
            // gst_element_get_state(pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
            if (ret != GST_STATE_CHANGE_FAILURE)
                activated = true;
        }
    }

    void deactivate()
    {
        if (!activated)
            return;

        const GstStateChangeReturn setResult = gst_element_set_state(pipeline, GST_STATE_NULL);
        if (setResult == GST_STATE_CHANGE_FAILURE) {
            qWarning("Pipeline failed to enter NULL during teardown");
        } else if (setResult == GST_STATE_CHANGE_ASYNC) {
            GstState                   current    = GST_STATE_VOID_PENDING;
            GstState                   pending    = GST_STATE_VOID_PENDING;
            const GstStateChangeReturn waitResult = gst_element_get_state(pipeline, &current, &pending, 2 * GST_SECOND);
            if (waitResult == GST_STATE_CHANGE_ASYNC) {
                qWarning("Pipeline teardown timed out after 2s (current=%d pending=%d)", int(current), int(pending));
            } else if (waitResult == GST_STATE_CHANGE_FAILURE) {
                qWarning("Pipeline teardown failed while waiting for NULL");
            }
        }
        activated = false;
    }
};

PipelineContext::PipelineContext() { d = new Private; }

PipelineContext::~PipelineContext() { delete d; }

void PipelineContext::activate() { d->activate(); }

void PipelineContext::deactivate() { d->deactivate(); }

GstElement *PipelineContext::element() { return d->pipeline; }

//----------------------------------------------------------------------------
// PipelineDeviceContext
//----------------------------------------------------------------------------
PipelineDeviceContext::PipelineDeviceContext() : d(new PipelineDeviceContextPrivate) { }

PipelineDeviceContext *PipelineDeviceContext::create(PipelineContext *pipeline, const QString &id, PDevice::Type type,
                                                     DeviceMonitor *deviceMonitor, const PipelineDeviceOptions &opts)
{
    auto that = new PipelineDeviceContext;

    that->d->pipeline  = pipeline;
    that->d->opts      = opts;
    that->d->activated = false;

    // see if we're already using this device, so we can attempt to share
    PipelineDevice *dev = nullptr;
    for (PipelineDevice *i : std::as_const(pipeline->d->devices)) {
        if (i->id == id && i->type == type) {
            dev = i;
            break;
        }
    }

    if (!dev) {
        dev = new PipelineDevice(id, type, that->d, deviceMonitor);
        if (!dev->device_bin) {
            delete dev;
            delete that;
            return nullptr;
        }
        that->d->opts.echoProberName = dev->echoProbeName();

        pipeline->d->devices += dev;
    } else {
        // FIXME: make sharing work
        // dev->addRef(that->d);

        delete that;
        return nullptr;
    }

    that->d->device = dev;

#ifdef PIPELINE_DEBUG
    qDebug("Readying %s:[%s], refs=%d", type_to_str(dev->type), qPrintable(dev->id), dev->refs);
#endif
    return that;
}

PipelineDeviceContext::~PipelineDeviceContext()
{
    PipelineDevice *dev = d->device;

    if (dev) {
        dev->removeRef(d);
#ifdef PIPELINE_DEBUG
        qDebug("Releasing %s:[%s], refs=%d", type_to_str(dev->type), qPrintable(dev->id), dev->refs);
#endif
        if (dev->refs == 0) {
            d->pipeline->d->devices.remove(dev);
            delete dev;
        }
    }

    delete d;
}

void PipelineDeviceContext::activate() { d->device->activate(d); }

void PipelineDeviceContext::deactivate() { d->device->deactivate(d); }

GstElement *PipelineDeviceContext::element() { return d->element; }

void PipelineDeviceContext::setOptions(const PipelineDeviceOptions &opts)
{
    d->opts = opts;
    d->device->update(*this);
}

PipelineDeviceOptions PipelineDeviceContext::options() const { return d->opts; }

}
