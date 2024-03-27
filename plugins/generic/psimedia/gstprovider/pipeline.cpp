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
#include <cstdio>
#include <gst/gst.h>

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
                             nullptr);
}

static GstCaps *filter_for_desired_size(const QSize &size)
{
    Q_UNUSED(size)
    //    QList<int> widths;
    //    widths << 160 << 320 << 640 << 800 << 1024;
    //    for(int n = 0; n < widths.count(); ++n)
    //    {
    //        if(widths[n] < size.width())
    //        {
    //            widths.removeAt(n);
    //            --n; // adjust position
    //        }
    //    }

    //    GstElement *capsfilter = gst_element_factory_make("capsfilter", nullptr);
    //    GstCaps *caps = gst_caps_new_empty();

    //     for(int n = 0; n < widths.count(); ++n)
    //     {
    //         GstStructure *cs;
    //         cs = gst_structure_new("video/x-raw-yuv",
    //             "width", GST_TYPE_INT_RANGE, 1, widths[n],
    //             "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, nullptr);
    //         gst_caps_append_structure(caps, cs);
    //
    //         cs = gst_structure_new("video/x-raw-rgb",
    //             "width", GST_TYPE_INT_RANGE, 1, widths[n],
    //             "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, nullptr);
    //         gst_caps_append_structure(caps, cs);
    //     }
    return gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 480, "framerate",
                               GST_TYPE_FRACTION, 30, 1, nullptr);
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
    PipelineContext      *pipeline;
    PipelineDevice       *device;
    PipelineDeviceOptions opts;
    bool                  activated;

    // queue for srcs, adder for sinks
    GstElement *element;
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
    GstElement *makeDeviceBin(const PipelineDeviceOptions &options)
    {
        QSize       captureSize;
        GstElement *deviceElement = devices_makeElement(id, type, &captureSize);
        if (!deviceElement)
            return nullptr;

        // explicitly set audio devices to be low-latency
        if (/*type == PDevice::AudioIn ||*/ type == PDevice::AudioOut) {
            int latency_ms = get_latency_time();
            if (latency_ms > 0) {
                gint64 lt = latency_ms * 1000; // microseconds
                g_object_set(G_OBJECT(deviceElement), "latency-time", lt, nullptr);
                // g_object_set(G_OBJECT(e), "buffer-time", 2 * lt, nullptr);
            }
        }

        GstElement *bin = gst_bin_new(nullptr); // FIXME not necessary for audio?

        if (type == PDevice::AudioIn) {
            aindev = deviceElement;
            GstPad *pad;
            gst_element_set_name(deviceElement, "aindev");
            gst_bin_add(GST_BIN(bin), deviceElement);

            if (options.aec) {

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
            GstCaps *capsfilter = nullptr;

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

            if (captureSize.isValid())
                capsfilter = filter_for_capture_size(captureSize);
            else if (options.videoSize.isValid())
                capsfilter = filter_for_desired_size(options.videoSize);

            gst_bin_add(GST_BIN(bin), deviceElement);

            GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
            gst_bin_add(GST_BIN(bin), decodebin);

            GstPad *pad
                = gst_ghost_pad_new_no_target_from_template("src", gst_static_pad_template_get(&videosrcbin_template));
            gst_element_add_pad(bin, pad);

            g_signal_connect(G_OBJECT(decodebin), "pad-added", G_CALLBACK(videosrcbin_pad_added), pad);

            if (capsfilter) {
                gst_element_link_filtered(deviceElement, decodebin, capsfilter);
                gst_caps_unref(capsfilter);
            } else {
                gst_element_link(deviceElement, decodebin);
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
    PipelineDevice(const QString &_id, PDevice::Type _type, PipelineDeviceContextPrivate *context) :
        refs(0), id(_id), type(_type)
    {
        pipeline = context->pipeline->element();

        device_bin = makeDeviceBin(context->opts);
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
            GstElement *queue = gst_element_factory_make("queue", nullptr);
            context->element  = queue;
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
        if (activated) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_element_get_state(pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);
            activated = false;
        }
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
PipelineDeviceContext::PipelineDeviceContext()
{
    d         = new PipelineDeviceContextPrivate;
    d->device = nullptr;
}

PipelineDeviceContext *PipelineDeviceContext::create(PipelineContext *pipeline, const QString &id, PDevice::Type type,
                                                     const PipelineDeviceOptions &opts)
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
        dev = new PipelineDevice(id, type, that->d);
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
