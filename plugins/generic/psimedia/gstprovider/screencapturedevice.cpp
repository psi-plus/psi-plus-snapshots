// SPDX-License-Identifier: LGPL-2.1-or-later
#include "screencapturedevice_p.h"
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QUuid>

namespace PsiMedia {
namespace {
    QMutex                                             deviceMutex;
    QHash<QString, std::weak_ptr<ScreenCaptureDevice>> devices;
}
std::shared_ptr<ScreenCaptureDevice> registerScreenCaptureDevice(const QString              &pipeline,
                                                                 std::shared_ptr<const void> resources)
{
    auto device       = std::make_shared<ScreenCaptureDevice>();
    device->id        = QStringLiteral("psimedia-screen:") + QUuid::createUuid().toString(QUuid::Id128);
    device->pipeline  = pipeline;
    device->resources = std::move(resources);
    QMutexLocker lock(&deviceMutex);
    devices.insert(device->id, device);
    return device;
}
void revokeScreenCaptureDevice(const QString &id)
{
    QMutexLocker lock(&deviceMutex);
    devices.remove(id);
}
std::shared_ptr<const ScreenCaptureDevice> screenCaptureDevice(const QString &id)
{
    QMutexLocker lock(&deviceMutex);
    return devices.value(id).lock();
}
GstElement *makeScreenCaptureElement(const QString &id)
{
    // Serialize construction against revocation. No capture starts while parsing
    // a source: the media worker still controls its state transitions.
    QMutexLocker lock(&deviceMutex);
    auto         device = devices.value(id).lock();
    if (!device)
        return nullptr;
    auto *element = gst_parse_launch(device->pipeline.toUtf8().constData(), nullptr);
    if (element) {
        static const GQuark key = g_quark_from_static_string("psimedia-screen-device");
        g_object_set_qdata_full(
            G_OBJECT(element), key, new std::shared_ptr<ScreenCaptureDevice>(std::move(device)),
            [](gpointer value) { delete static_cast<std::shared_ptr<ScreenCaptureDevice> *>(value); });
    }
    return element;
}
}
