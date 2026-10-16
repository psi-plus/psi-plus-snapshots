// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <gst/gst.h>

// Private bridge to the Objective-C++ ScreenCaptureKit implementation.
// The source owns the returned controller and releases it with stopScreenCaptureKit().
// Tests substitute this bridge to exercise the appsrc lifecycle without macOS consent.
void *startScreenCaptureKit(GstElement *source, unsigned display, unsigned window);
void  stopScreenCaptureKit(void *capture);
