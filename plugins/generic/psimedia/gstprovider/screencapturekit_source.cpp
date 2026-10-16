// SPDX-License-Identifier: LGPL-2.1-or-later
#include "screencapturekit_source.h"
#include "screencapturekit_capture_p.h"
#include <gst/app/gstappsrc.h>
#include <mutex>

struct GstPsiScreenCaptureKitSrc {
    GstAppSrc parent;
    guint     displayId, windowId;
    void     *controller;
};
struct GstPsiScreenCaptureKitSrcClass {
    GstAppSrcClass parent;
};
G_DEFINE_TYPE(GstPsiScreenCaptureKitSrc, gst_psi_screen_capture_kit_src, GST_TYPE_APP_SRC)

enum { PropertyNone, PropertyDisplay, PropertyWindow };
static gboolean screenStart(GstBaseSrc *base)
{
    auto *self = reinterpret_cast<GstPsiScreenCaptureKitSrc *>(base);
    if ((self->displayId == 0) == (self->windowId == 0)) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, ("Select exactly one display or window"), (nullptr));
        return FALSE;
    }
    if (!GST_BASE_SRC_CLASS(gst_psi_screen_capture_kit_src_parent_class)->start(base))
        return FALSE;
    self->controller = startScreenCaptureKit(GST_ELEMENT(self), self->displayId, self->windowId);
    if (self->controller)
        return TRUE;
    GST_BASE_SRC_CLASS(gst_psi_screen_capture_kit_src_parent_class)->stop(base);
    return FALSE;
}
static void stopController(GstPsiScreenCaptureKitSrc *self)
{
    stopScreenCaptureKit(self->controller);
    self->controller = nullptr;
}
static gboolean screenStop(GstBaseSrc *base)
{
    stopController(reinterpret_cast<GstPsiScreenCaptureKitSrc *>(base));
    return GST_BASE_SRC_CLASS(gst_psi_screen_capture_kit_src_parent_class)->stop(base);
}
static void screenFinalize(GObject *object)
{
    stopController(reinterpret_cast<GstPsiScreenCaptureKitSrc *>(object));
    G_OBJECT_CLASS(gst_psi_screen_capture_kit_src_parent_class)->finalize(object);
}
static void screenSetProperty(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    auto *self = reinterpret_cast<GstPsiScreenCaptureKitSrc *>(object);
    if (id == PropertyDisplay)
        self->displayId = g_value_get_uint(value);
    else if (id == PropertyWindow)
        self->windowId = g_value_get_uint(value);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void screenGetProperty(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    auto *self = reinterpret_cast<GstPsiScreenCaptureKitSrc *>(object);
    if (id == PropertyDisplay)
        g_value_set_uint(value, self->displayId);
    else if (id == PropertyWindow)
        g_value_set_uint(value, self->windowId);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void gst_psi_screen_capture_kit_src_class_init(GstPsiScreenCaptureKitSrcClass *klass)
{
    auto *object         = G_OBJECT_CLASS(klass);
    object->set_property = screenSetProperty;
    object->get_property = screenGetProperty;
    object->finalize     = screenFinalize;
    auto *base           = GST_BASE_SRC_CLASS(klass);
    base->start          = screenStart;
    base->stop           = screenStop;
    g_object_class_install_property(object, PropertyDisplay,
                                    g_param_spec_uint("display-id", "Display ID", "Selected ScreenCaptureKit display",
                                                      0, G_MAXUINT, 0,
                                                      GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object, PropertyWindow,
                                    g_param_spec_uint("window-id", "Window ID", "Selected ScreenCaptureKit window", 0,
                                                      G_MAXUINT, 0,
                                                      GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass), "Psi ScreenCaptureKit source", "Source/Video",
                                          "Captures an explicitly selected macOS display or window", "Psi developers");
}
static void gst_psi_screen_capture_kit_src_init(GstPsiScreenCaptureKitSrc *self)
{
    self->displayId = self->windowId = 0;
    self->controller                 = nullptr;
    g_object_set(self, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE, "block", FALSE, "max-buffers",
                 guint64(2), "max-bytes", guint64(1920 * 1080 * 4 * 2), "leaky-type", 2, nullptr);
}
bool registerScreenCaptureKitSource()
{
    static std::mutex           mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto *factory = gst_element_factory_find("psiscreencapturekitsrc")) {
        const bool same = gst_element_factory_get_element_type(factory) == gst_psi_screen_capture_kit_src_get_type();
        gst_object_unref(factory);
        return same;
    }
    return gst_element_register(nullptr, "psiscreencapturekitsrc", GST_RANK_NONE,
                                gst_psi_screen_capture_kit_src_get_type());
}
