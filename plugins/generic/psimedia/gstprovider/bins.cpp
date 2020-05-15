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

#include "bins.h"

#include <QSize>
#include <QString>
#include <cstdio>
#include <gst/gst.h>

// default latency is 200ms
#define DEFAULT_RTP_LATENCY 200

namespace PsiMedia {

static int get_rtp_latency()
{
    QString val = QString::fromLatin1(qgetenv("PSI_RTP_LATENCY"));
    if (!val.isEmpty())
        return val.toInt();
    else
        return DEFAULT_RTP_LATENCY;
}

static GstElement *audio_codec_to_enc_element(const QString &name)
{
    QString ename;
    if (name == "opus") {
        auto e = gst_element_factory_make("opusenc", "opus-encoder");
        gst_util_set_object_arg(G_OBJECT(e), "audio-type", "voice");
        gst_util_set_object_arg(G_OBJECT(e), "bitrate-type", "vbr");
        return e;
    } else if (name == "vorbis")
        ename = "vorbisenc";
    else if (name == "pcmu")
        ename = "mulawenc";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *audio_codec_to_dec_element(const QString &name)
{
    QString ename;
    if (name == "opus")
        ename = "opusdec";
    else if (name == "vorbis")
        ename = "vorbisdec";
    else if (name == "pcmu")
        ename = "mulawdec";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *audio_codec_to_rtppay_element(const QString &name)
{
    QString ename;
    if (name == "opus")
        ename = "rtpopuspay";
    else if (name == "vorbis")
        ename = "rtpvorbispay";
    else if (name == "pcmu")
        ename = "rtppcmupay";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *audio_codec_to_rtpdepay_element(const QString &name)
{
    QString ename;
    if (name == "opus")
        ename = "rtpopusdepay";
    else if (name == "vorbis")
        ename = "rtpvorbisdepay";
    else if (name == "pcmu")
        ename = "rtppcmudepay";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *video_codec_to_enc_element(const QString &name)
{
    QString ename;
    if (name == "theora")
        ename = "theoraenc";
    else if (name == "h263p")
        ename = "ffenc_h263p";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *video_codec_to_dec_element(const QString &name)
{
    QString ename;
    if (name == "theora")
        ename = "theoradec";
    else if (name == "h263p")
        ename = "ffdec_h263";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *video_codec_to_rtppay_element(const QString &name)
{
    QString ename;
    if (name == "theora")
        ename = "rtptheorapay";
    else if (name == "h263p")
        ename = "rtph263ppay";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static GstElement *video_codec_to_rtpdepay_element(const QString &name)
{
    QString ename;
    if (name == "theora")
        ename = "rtptheoradepay";
    else if (name == "h263p")
        ename = "rtph263pdepay";
    else
        return nullptr;

    return gst_element_factory_make(ename.toLatin1().data(), nullptr);
}

static bool audio_codec_get_send_elements(const QString &name, GstElement **enc, GstElement **rtppay)
{
    GstElement *eenc = audio_codec_to_enc_element(name);
    if (!eenc)
        return false;
    GstElement *epay = audio_codec_to_rtppay_element(name);
    if (!epay) {
        g_object_unref(G_OBJECT(eenc));
    }

    *enc    = eenc;
    *rtppay = epay;
    return true;
}

static bool audio_codec_get_recv_elements(const QString &name, GstElement **dec, GstElement **rtpdepay)
{
    GstElement *edec = audio_codec_to_dec_element(name);
    if (!edec)
        return false;
    GstElement *edepay = audio_codec_to_rtpdepay_element(name);
    if (!edepay) {
        g_object_unref(G_OBJECT(edec));
    }

    *dec      = edec;
    *rtpdepay = edepay;
    return true;
}

static bool video_codec_get_send_elements(const QString &name, GstElement **enc, GstElement **rtppay)
{
    GstElement *eenc = video_codec_to_enc_element(name);
    if (!eenc)
        return false;
    GstElement *epay = video_codec_to_rtppay_element(name);
    if (!epay) {
        g_object_unref(G_OBJECT(eenc));
    }

    *enc    = eenc;
    *rtppay = epay;
    return true;
}

static bool video_codec_get_recv_elements(const QString &name, GstElement **dec, GstElement **rtpdepay)
{
    GstElement *edec = video_codec_to_dec_element(name);
    if (!edec)
        return false;
    GstElement *edepay = video_codec_to_rtpdepay_element(name);
    if (!edepay) {
        g_object_unref(G_OBJECT(edec));
    }

    *dec      = edec;
    *rtpdepay = edepay;
    return true;
}

GstElement *bins_videoprep_create(const QSize &size, int fps, bool is_live)
{
    Q_UNUSED(is_live);
    GstElement *bin = gst_bin_new("videoprepbin");

    GstElement *videorate  = nullptr;
    GstElement *ratefilter = nullptr;
    if (fps != -1) {
        videorate = gst_element_factory_make("videorate", nullptr);

        ratefilter = gst_element_factory_make("capsfilter", nullptr);

        GstCaps *     caps = gst_caps_new_empty();
        GstStructure *cs   = gst_structure_new("video/x-raw", "framerate", GST_TYPE_FRACTION, fps, 1, NULL);

        gst_caps_append_structure(caps, cs);

        g_object_set(G_OBJECT(ratefilter), "caps", caps, NULL);
        gst_caps_unref(caps);
    }

    GstElement *videoscale  = nullptr;
    GstElement *scalefilter = nullptr;
    if (size.isValid()) {
        videoscale  = gst_element_factory_make("videoscale", nullptr);
        scalefilter = gst_element_factory_make("capsfilter", nullptr);

        GstCaps *     caps = gst_caps_new_empty();
        GstStructure *cs   = gst_structure_new("video/x-raw", "width", G_TYPE_INT, size.width(), "height", G_TYPE_INT,
                                             size.height(), NULL);

        gst_caps_append_structure(caps, cs);

        g_object_set(G_OBJECT(scalefilter), "caps", caps, NULL);
        gst_caps_unref(caps);
    }

    if (!videorate && !videoscale) {
        // not altering anything?  return no-op
        return gst_element_factory_make("identity", nullptr);
    }

    GstElement *start, *end;
    if (videorate && videoscale) {
        start = videorate;
        end   = scalefilter;
    } else if (videorate && !videoscale) {
        start = videorate;
        end   = ratefilter;
    } else // !videorate && videoscale
    {
        start = videoscale;
        end   = scalefilter;
    }

    if (videorate) {
        gst_bin_add(GST_BIN(bin), videorate);
        gst_bin_add(GST_BIN(bin), ratefilter);
        gst_element_link(videorate, ratefilter);
    }

    if (videoscale) {
        gst_bin_add(GST_BIN(bin), videoscale);
        gst_bin_add(GST_BIN(bin), scalefilter);
        gst_element_link(videoscale, scalefilter);
    }

    if (videorate && videoscale)
        gst_element_link(ratefilter, videoscale);

    GstPad *pad;

    pad = gst_element_get_static_pad(start, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(end, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(GST_OBJECT(pad));

    return bin;
}

GstElement *bins_audioenc_create(const QString &codec, int id, int rate, int size, int channels)
{
    bool variableRate = (codec == QLatin1String("opus")); // opus supports variable bitrate and resampling on its own
    GstElement *bin   = gst_bin_new("audioencbin");

    GstElement *audioenc    = nullptr;
    GstElement *audiortppay = nullptr;
    if (!audio_codec_get_send_elements(codec, &audioenc, &audiortppay))
        return nullptr;

    if (id != -1)
        g_object_set(G_OBJECT(audiortppay), "pt", id, NULL);

    GstElement *audioconvert  = gst_element_factory_make("audioconvert", nullptr);
    GstElement *audioresample = nullptr;
    if (!variableRate) {
        // suppose variadic-rate encoder have internal resampler (like opus)
        audioresample = gst_element_factory_make("audioresample", nullptr);
    }

    GstStructure *cs;
    GstCaps *     caps = gst_caps_new_empty();
    if (variableRate) {
        // there is much sense to change rate if variadic-rate codec can do internal resampling.
        // also width could be taken from internal codec's caps. just any width.
        cs = gst_structure_new("audio/x-raw", "channels", G_TYPE_INT, channels, "channel-mask", GST_TYPE_BITMASK, 1,
                               NULL);
        qDebug("channels=%d", channels);
    } else {
        cs = gst_structure_new("audio/x-raw", "rate", G_TYPE_INT, rate, "width", G_TYPE_INT, size, "channels",
                               G_TYPE_INT, channels, "channel-mask", GST_TYPE_BITMASK, 1, NULL);
        qDebug("rate=%d,width=%d,channels=%d", rate, size, channels);
    }
    gst_caps_append_structure(caps, cs);
    GstElement *capsfilter = gst_element_factory_make("capsfilter", nullptr);
    g_object_set(G_OBJECT(capsfilter), "caps", caps, NULL);
    gst_caps_unref(caps);

    gst_bin_add(GST_BIN(bin), audioconvert);
    if (audioresample) {
        gst_bin_add(GST_BIN(bin), audioresample);
    }
    gst_bin_add(GST_BIN(bin), capsfilter);
    gst_bin_add(GST_BIN(bin), audioenc);
    gst_bin_add(GST_BIN(bin), audiortppay);

    if (audioresample) {
        gst_element_link_many(audioconvert, audioresample, capsfilter, audioenc, audiortppay, NULL);
    } else {
        gst_element_link_many(audioconvert, capsfilter, audioenc, audiortppay, NULL);
    }

    GstPad *pad;

    pad = gst_element_get_static_pad(audioconvert, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(audiortppay, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(GST_OBJECT(pad));

    return bin;
}

GstElement *bins_videoenc_create(const QString &codec, int id, int maxkbps)
{
    GstElement *bin = gst_bin_new("videoencbin");

    GstElement *videoenc    = nullptr;
    GstElement *videortppay = nullptr;
    if (!video_codec_get_send_elements(codec, &videoenc, &videortppay))
        return nullptr;

    if (id != -1)
        g_object_set(G_OBJECT(videortppay), "pt", id, NULL);

    if (codec == "theora")
        g_object_set(G_OBJECT(videoenc), "bitrate", maxkbps, NULL);

    GstElement *videoconvert = gst_element_factory_make("videoconvert", nullptr);

    gst_bin_add(GST_BIN(bin), videoconvert);
    gst_bin_add(GST_BIN(bin), videoenc);
    gst_bin_add(GST_BIN(bin), videortppay);

    gst_element_link_many(videoconvert, videoenc, videortppay, NULL);

    GstPad *pad;

    pad = gst_element_get_static_pad(videoconvert, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(videortppay, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(GST_OBJECT(pad));

    return bin;
}

GstElement *bins_audiodec_create(const QString &codec)
{
    GstElement *bin = gst_bin_new("audiodecbin");

    GstElement *audiodec      = nullptr;
    GstElement *audiortpdepay = nullptr;
    if (!audio_codec_get_recv_elements(codec, &audiodec, &audiortpdepay))
        return nullptr;

    GstElement *audiortpjitterbuffer = gst_element_factory_make("rtpjitterbuffer", nullptr);

    gst_bin_add(GST_BIN(bin), audiortpjitterbuffer);
    gst_bin_add(GST_BIN(bin), audiortpdepay);
    gst_bin_add(GST_BIN(bin), audiodec);

    gst_element_link_many(audiortpjitterbuffer, audiortpdepay, audiodec, NULL);

    g_object_set(G_OBJECT(audiortpjitterbuffer), "latency", (unsigned int)get_rtp_latency(), NULL);

    GstPad *pad;

    pad = gst_element_get_static_pad(audiortpjitterbuffer, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(audiodec, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(GST_OBJECT(pad));

    return bin;
}

GstElement *bins_videodec_create(const QString &codec)
{
    GstElement *bin = gst_bin_new("videodecbin");

    GstElement *videodec      = nullptr;
    GstElement *videortpdepay = nullptr;
    if (!video_codec_get_recv_elements(codec, &videodec, &videortpdepay))
        return nullptr;

    GstElement *videortpjitterbuffer = gst_element_factory_make("rtpjitterbuffer", nullptr);

    gst_bin_add(GST_BIN(bin), videortpjitterbuffer);
    gst_bin_add(GST_BIN(bin), videortpdepay);
    gst_bin_add(GST_BIN(bin), videodec);

    gst_element_link_many(videortpjitterbuffer, videortpdepay, videodec, NULL);

    g_object_set(G_OBJECT(videortpjitterbuffer), "latency", (unsigned int)get_rtp_latency(), NULL);

    GstPad *pad;

    pad = gst_element_get_static_pad(videortpjitterbuffer, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    pad = gst_element_get_static_pad(videodec, "src");
    gst_element_add_pad(bin, gst_ghost_pad_new("src", pad));
    gst_object_unref(GST_OBJECT(pad));

    return bin;
}

}
