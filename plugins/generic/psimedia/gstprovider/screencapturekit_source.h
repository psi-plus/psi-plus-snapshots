// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// Called after gst_init(), on the provider's GStreamer thread. Registration
// itself never requests Screen Recording permission or starts capture.
bool registerScreenCaptureKitSource();
