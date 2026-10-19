// SPDX-License-Identifier: LGPL-2.1-or-later
// Regression: the AEC input caps must describe positionless mono, not FRONT_LEFT.
#include "pipeline.h"

#include <QCoreApplication>
#include <QDebug>

#include <gst/audio/audio.h>
#include <gst/gst.h>

#include <memory>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(&argc, &argv);

    // No microphone or audio sink is needed. The element factory might be
    // absent on distributions without gst-plugins-bad.
    GstElementFactory *dspFactory = gst_element_factory_find("webrtcdsp");
    if (!dspFactory) {
        qInfo() << "SKIP: webrtcdsp plugin is not installed";
        return 0;
    }
    gst_object_unref(dspFactory);

    PsiMedia::PipelineContext pipeline;
    PsiMedia::PipelineDeviceOptions options;
    options.aec = true;
    options.echoProberName = QStringLiteral("test_echo_probe");

    auto device = std::unique_ptr<PsiMedia::PipelineDeviceContext>(
        PsiMedia::PipelineDeviceContext::create(&pipeline, QStringLiteral("audiotestsrc"),
                                                PsiMedia::PDevice::AudioIn, nullptr, options));
    if (!device) {
        qCritical() << "Failed to construct AEC capture source";
        return 1;
    }

    GstElement *filter = gst_bin_get_by_name(GST_BIN(pipeline.element()), "webrtcdsp_mono_caps");
    if (!filter) {
        qCritical() << "AEC mono filter missing from capture pipeline";
        return 2;
    }

    GstCaps *caps = nullptr;
    g_object_get(filter, "caps", &caps, nullptr);
    gst_object_unref(filter);

    const GstStructure *structure = caps && gst_caps_get_size(caps) == 1 ? gst_caps_get_structure(caps, 0) : nullptr;
    gint rate = 0, channels = 0;
    const gchar *format = structure ? gst_structure_get_string(structure, "format") : nullptr;
    // capsfilter leaves layout negotiable. Fixate it only for the GstAudioInfo
    // parser, keeping the actual production filter unrestricted.
    GstCaps *fixedCaps = caps ? gst_caps_copy(caps) : nullptr;
    if (fixedCaps)
        gst_structure_set(gst_caps_get_structure(fixedCaps, 0), "layout", G_TYPE_STRING, "interleaved", nullptr);
    GstAudioInfo audioInfo;
    const bool ok = structure && gst_structure_get_int(structure, "rate", &rate)
        && rate == 48000 && gst_structure_get_int(structure, "channels", &channels) && channels == 1
        && format && g_strcmp0(format, "S16LE") == 0
        && !gst_structure_has_field(structure, "channel-mask")
        && gst_audio_info_from_caps(&audioInfo, fixedCaps)
        && GST_AUDIO_INFO_CHANNELS(&audioInfo) == 1
        && GST_AUDIO_INFO_POSITION(&audioInfo, 0) == GST_AUDIO_CHANNEL_POSITION_MONO;

    if (!ok) {
        gchar *text = caps ? gst_caps_to_string(caps) : nullptr;
        qCritical() << "Invalid AEC mono caps:" << (text ? text : "<null>");
        g_free(text);
    }
    if (fixedCaps)
        gst_caps_unref(fixedCaps);
    if (caps)
        gst_caps_unref(caps);
    return ok ? 0 : 3;
}
