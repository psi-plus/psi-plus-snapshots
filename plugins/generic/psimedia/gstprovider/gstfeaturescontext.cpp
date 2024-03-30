#include "gstfeaturescontext.h"

#include "devices.h"
#include "gstthread.h"
#include "modes.h"

#include <QThread>

namespace PsiMedia {

GstFeaturesContext::GstFeaturesContext(GstMainLoop *_gstLoop, DeviceMonitor *deviceMonitor, QObject *parent) :
    QObject(parent), gstLoop(_gstLoop), deviceMonitor(deviceMonitor)
{
    Q_ASSERT(!gstLoop.isNull());
    // note deviceMonitor works in gstloop's thread
    connect(this->deviceMonitor, &DeviceMonitor::updated, this, &GstFeaturesContext::updateDevices);
    updateDevices();
    gstLoop->execInContext([this](void *) { this->deviceMonitor->start(); }, nullptr);
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
    for (const GstDevice &i : deviceMonitor->devices(PDevice::AudioOut))
        list += i.toPDevice();
    return list;
}

QList<PDevice> GstFeaturesContext::audioInputDevices()
{
    QList<PDevice> list;
    if (!deviceMonitor) {
        qCritical("device monitor is not initialized or destroyed");
        return list;
    }
    for (const GstDevice &i : deviceMonitor->devices(PDevice::AudioIn))
        list += i.toPDevice();
    return list;
}

QList<PDevice> GstFeaturesContext::videoInputDevices()
{
    QList<PDevice> list;
    if (!deviceMonitor) {
        qCritical("device monitor is not initialized or destroyed");
        return list;
    }
    for (const GstDevice &i : deviceMonitor->devices(PDevice::VideoIn))
        list += i.toPDevice();
    return list;
}

void GstFeaturesContext::updateDevices()
{
    qDebug("GstFeaturesContext::updateDevices thread=%p", QThread::currentThreadId());
    updated                      = true;
    features.audioInputDevices   = audioInputDevices();
    features.audioOutputDevices  = audioOutputDevices();
    features.videoInputDevices   = videoInputDevices();
    features.supportedAudioModes = modes_supportedAudio();
    features.supportedVideoModes = modes_supportedVideo();
    watch();
}

} // namespace PsiMedia
