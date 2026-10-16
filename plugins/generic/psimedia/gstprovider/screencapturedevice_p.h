// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QString>
#include <gst/gst.h>
#include <memory>

namespace PsiMedia {
// A selected source's lifetime is separate from its permission. Revocation
// prevents new graph construction; existing elements retain resources to NULL.
struct ScreenCaptureDevice {
    QString                     id;
    QString                     pipeline;
    std::shared_ptr<const void> resources;
};
std::shared_ptr<ScreenCaptureDevice>       registerScreenCaptureDevice(const QString              &pipeline,
                                                                       std::shared_ptr<const void> resources = {});
void                                       revokeScreenCaptureDevice(const QString &id);
std::shared_ptr<const ScreenCaptureDevice> screenCaptureDevice(const QString &id);
GstElement                                *makeScreenCaptureElement(const QString &id);
}
