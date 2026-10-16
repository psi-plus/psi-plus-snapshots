// SPDX-License-Identifier: LGPL-2.1-or-later
#include "screencapturekit_source.h"
#include "screencapturekit_capture_p.h"
#include <gst/app/gstappsrc.h>
#include <initializer_list>
static int starts = 0, stops = 0;
void      *startScreenCaptureKit(GstElement *, unsigned display, unsigned window)
{
    g_assert((display != 0) != (window != 0));
    ++starts;
    return &starts;
}
void stopScreenCaptureKit(void *capture)
{
    if (capture)
        ++stops;
}
int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    g_assert(registerScreenCaptureKitSource());
    g_assert(registerScreenCaptureKitSource());
    GstElement *source = gst_element_factory_make("psiscreencapturekitsrc", nullptr);
    g_assert(source && GST_IS_APP_SRC(source));
    guint64  buffers = 0;
    gboolean live = FALSE, block = TRUE;
    gint     leaky = 0;
    g_object_get(source, "max-buffers", &buffers, "is-live", &live, "block", &block, "leaky-type", &leaky, nullptr);
    g_assert_cmpuint(buffers, ==, 2);
    g_assert(live && !block && leaky == 2);
    // Merely constructing/enumerating a source cannot start native recording.
    g_assert_cmpint(starts, ==, 0);
    g_object_set(source, "display-id", 123u, nullptr);
    g_assert(gst_element_set_state(source, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE);
    g_assert_cmpint(starts, ==, 1);
    gst_element_set_state(source, GST_STATE_NULL);
    g_assert_cmpint(stops, ==, 1);
    // Reusing the source starts a fresh native stream; old state is not retained.
    g_object_set(source, "display-id", 0u, "window-id", 456u, nullptr);
    g_assert(gst_element_set_state(source, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE);
    g_assert_cmpint(starts, ==, 2);
    gst_element_set_state(source, GST_STATE_NULL);
    g_assert_cmpint(stops, ==, 2);
    gst_object_unref(source);
    // No implicit default display, and no ambiguous two-source selection.
    for (bool both : { false, true }) {
        source = gst_element_factory_make("psiscreencapturekitsrc", nullptr);
        if (both)
            g_object_set(source, "display-id", 123u, "window-id", 456u, nullptr);
        g_assert(gst_element_set_state(source, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE);
        gst_element_set_state(source, GST_STATE_NULL);
        gst_object_unref(source);
    }
    g_assert_cmpint(starts, ==, 2);
    gst_deinit();
    return 0;
}
