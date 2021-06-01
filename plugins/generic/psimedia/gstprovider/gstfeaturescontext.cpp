#include "gstfeaturescontext.h"

#include "devices.h"
#include "gstthread.h"
#include "modes.h"

namespace PsiMedia {

static PDevice gstDeviceToPDevice(const GstDevice &dev, PDevice::Type type)
{
    PDevice out;
    out.type      = type;
    out.name      = dev.name;
    out.id        = dev.id;
    out.isDefault = dev.isDefault;
    return out;
}

GstFeaturesContext::GstFeaturesContext(GstMainLoop *_gstLoop, DeviceMonitor *deviceMonitor, QObject *parent) :
    QObject(parent), gstLoop(_gstLoop), deviceMonitor(deviceMonitor)
{
    Q_ASSERT(!gstLoop.isNull());
    gstLoop->execInContext(
        [this](void *userData) {
            Q_UNUSED(userData);
            // we should set flags which exactly devices were 'updated'. will be implemenented later
            connect(this->deviceMonitor, &DeviceMonitor::updated, this, [this]() { updateDevices(); });
            updateDevices();
        },
        this);
}

QObject *GstFeaturesContext::qobject() { return this; }

void GstFeaturesContext::lookup(int types, QObject *receiver, std::function<void(const PFeatures &)> &&callback)
{
    watchers.emplace_back(types, true, QPointer<QObject>(receiver), std::move(callback));
    watch();
}

void GstFeaturesContext::monitor(int types, QObject *receiver, std::function<void(const PFeatures &)> &&callback)
{
    watchers.emplace_back(types, false, QPointer<QObject>(receiver), std::move(callback));
}

void GstFeaturesContext::watch()
{
    QMutexLocker locker(&updateMutex);
    if (!updated)
        return;
    auto it = watchers.cbegin();
    while (it != watchers.cend()) {
        if (!it->context) {
            it = watchers.erase(it);
            continue;
        }
        // we should check updated flags/types here when implemented
        it->callback(features);
        if (it->oneShot) {
            it = watchers.erase(it);
            continue;
        }
        ++it;
    }
}

QList<PDevice> GstFeaturesContext::audioOutputDevices()
{
    QList<PDevice> list;
    if (!deviceMonitor) {
        qCritical("device monitor is not initialized or destroyed");
        return list;
    }
    foreach (const GstDevice &i, deviceMonitor->devices(PDevice::AudioOut))
        list += gstDeviceToPDevice(i, PDevice::AudioOut);
    return list;
}

QList<PDevice> GstFeaturesContext::audioInputDevices()
{
    QList<PDevice> list;
    if (!deviceMonitor) {
        qCritical("device monitor is not initialized or destroyed");
        return list;
    }
    foreach (const GstDevice &i, deviceMonitor->devices(PDevice::AudioIn))
        list += gstDeviceToPDevice(i, PDevice::AudioIn);
    return list;
}

QList<PDevice> GstFeaturesContext::videoInputDevices()
{
    QList<PDevice> list;
    if (!deviceMonitor) {
        qCritical("device monitor is not initialized or destroyed");
        return list;
    }
    foreach (const GstDevice &i, deviceMonitor->devices(PDevice::VideoIn))
        list += gstDeviceToPDevice(i, PDevice::VideoIn);
    return list;
}

void GstFeaturesContext::updateDevices()
{
    QMutexLocker locker(&updateMutex);
    updated                      = true;
    features.audioInputDevices   = audioInputDevices();
    features.audioOutputDevices  = audioOutputDevices();
    features.videoInputDevices   = videoInputDevices();
    features.supportedAudioModes = modes_supportedAudio();
    features.supportedVideoModes = modes_supportedVideo();
    QMetaObject::invokeMethod(this, "watch", Qt::QueuedConnection);
}

} // namespace PsiMedia
