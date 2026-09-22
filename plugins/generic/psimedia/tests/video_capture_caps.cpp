// SPDX-License-Identifier: LGPL-2.1-or-later

#include "../gstprovider/pipeline.h"

#include <QCoreApplication>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <gst/gst.h>

namespace {

void expectMode(const QString &nativeCaps, const QString &expectedMime, int width, int height, int fpsNum, int fpsDen)
{
    QString  selectedMime;
    GstCaps *caps = PsiMedia::selectVideoCaptureCaps(nativeCaps, QSize(640, 480), 30, &selectedMime);
    if (!caps)
        qFatal("Capture selector returned no caps");

    if (selectedMime != expectedMime)
        qFatal("Unexpected selected capture MIME");

    if (gst_caps_get_size(caps) != 1)
        qFatal("Capture selector returned ambiguous caps");

    const GstStructure *s            = gst_caps_get_structure(caps, 0);
    int                 actualWidth  = 0;
    int                 actualHeight = 0;
    int                 actualFpsNum = 0;
    int                 actualFpsDen = 0;
    if (!gst_structure_get_int(s, "width", &actualWidth) || !gst_structure_get_int(s, "height", &actualHeight)
        || !gst_structure_get_fraction(s, "framerate", &actualFpsNum, &actualFpsDen)) {
        gst_caps_unref(caps);
        qFatal("Selected capture caps are not fixed");
    }

    gst_caps_unref(caps);

    if (actualWidth != width || actualHeight != height || actualFpsNum != fpsNum || actualFpsDen != fpsDen)
        qFatal("Unexpected selected capture geometry/cadence");
}

void expectSanitizedPipeWireCaps()
{
    QString  selectedMime;
    GstCaps *caps = PsiMedia::selectVideoCaptureCaps(
        QStringLiteral("video/x-raw(memory:DMABuf),format=(string){ NV12, YUY2 },"
                       "width=(int)[320,1920],height=(int)[240,1080],framerate=(fraction)[5/1,30/1]"),
        QSize(640, 480), 30, &selectedMime);
    if (!caps)
        qFatal("Capture selector returned no caps for PipeWire feature caps");
    if (selectedMime != QStringLiteral("video/x-raw"))
        qFatal("Unexpected MIME for PipeWire feature caps");

    const GstStructure    *s        = gst_caps_get_structure(caps, 0);
    const GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    if (gst_caps_features_contains(features, "memory:DMABuf")) {
        gst_caps_unref(caps);
        qFatal("Discovery-only DMABuf feature leaked into source filter");
    }
    if (gst_structure_has_field(s, "format")) {
        gst_caps_unref(caps);
        qFatal("Discovery-only format list leaked into source filter");
    }

    int width  = 0;
    int height = 0;
    int fpsNum = 0;
    int fpsDen = 0;
    if (!gst_structure_get_int(s, "width", &width) || !gst_structure_get_int(s, "height", &height)
        || !gst_structure_get_fraction(s, "framerate", &fpsNum, &fpsDen) || width != 640 || height != 480
        || fpsNum != 30 || fpsDen != 1) {
        gst_caps_unref(caps);
        qFatal("Sanitized PipeWire caps lost the selected fixed mode");
    }
    gst_caps_unref(caps);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    gst_init(nullptr, nullptr);

    // PipeWire commonly collapses V4L2 discrete modes into ranges. The
    // selector must fixate near the call target instead of falling back to an
    // arbitrary low-rate tuple.
    expectMode(
        QStringLiteral("video/x-raw,width=(int)[320,1920],height=(int)[240,1080],framerate=(fraction)[5/1,30/1]"),
        QStringLiteral("video/x-raw"), 640, 480, 30, 1);

    // Smooth exact-size MJPEG must beat an unrelated 5 fps raw mode. This is
    // representative of USB webcams that expose richer compressed modes.
    expectMode(QStringLiteral("video/x-raw,width=(int)1920,height=(int)1080,framerate=(fraction)5/1;"
                              "image/jpeg,width=(int)640,height=(int)480,framerate=(fraction)30/1"),
               QStringLiteral("image/jpeg"), 640, 480, 30, 1);

    // For equal 4:3 geometry, a nearby higher-rate mode is preferable to an
    // exact but unusably slow 5 fps mode; downstream scaling keeps the canvas.
    expectMode(QStringLiteral("video/x-raw,width=(int)640,height=(int)480,framerate=(fraction)5/1;"
                              "video/x-raw,width=(int)800,height=(int)600,framerate=(fraction)20/1"),
               QStringLiteral("video/x-raw"), 800, 600, 20, 1);

    // With aspect-preserving downstream scaling, a smooth 16:9 source is still
    // preferable to an exact 4:3 mode stuck at 5 fps.
    expectMode(QStringLiteral("video/x-raw,width=(int)640,height=(int)480,framerate=(fraction)5/1;"
                              "video/x-raw,width=(int)1280,height=(int)720,framerate=(fraction)30/1"),
               QStringLiteral("video/x-raw"), 1280, 720, 30, 1);

    // Device discovery caps may carry PipeWire memory features and format
    // lists that are not safe to reuse as a live source filter.
    expectSanitizedPipeWireCaps();

    qInfo("Video capture caps selection regression passed");
    return 0;
}
