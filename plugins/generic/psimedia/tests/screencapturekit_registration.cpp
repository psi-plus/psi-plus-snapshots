// SPDX-License-Identifier: LGPL-2.1-or-later
#include "screencapturekit_source.h"
#include <gst/app/gstappsrc.h>
int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    g_assert(registerScreenCaptureKitSource());
    GstElement *source = gst_element_factory_make("psiscreencapturekitsrc", nullptr);
    g_assert(source && GST_IS_APP_SRC(source));
    g_object_set(source, "display-id", 123u, nullptr);
    guint    display = 0;
    guint64  limit   = 0;
    gboolean live    = FALSE;
    g_object_get(source, "display-id", &display, "max-buffers", &limit, "is-live", &live, nullptr);
    g_assert_cmpuint(display, ==, 123);
    g_assert_cmpuint(limit, ==, 2);
    g_assert(live);
    gst_object_unref(source);
    gst_deinit();
    return 0;
}
