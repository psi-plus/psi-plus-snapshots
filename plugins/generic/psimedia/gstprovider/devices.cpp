/*
 * Copyright (C) 2009-2024  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "devices.h"

#include "gstthread.h"
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QSize>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <gst/gst.h>

namespace PsiMedia {

#if !defined(Q_OS_LINUX)
// add more platforms to the ifdef when ready
// below is a default impl
QList<GstDevice> PlatformDeviceMonitor::getDevices() { return QList<GstDevice>(); }
#endif

// copied from gst-inspect-1.0. perfect for identifying devices
// https://github.com/freedesktop/gstreamer-gst-plugins-base/blob/master/tools/gst-device-monitor.c
static gchar *get_launch_line(::GstDevice *device)
{
    static const char *const ignored_propnames[] = { "name", "parent", "direction", "template", "caps", nullptr };
    GString                 *launch_line;
    GstElement              *element;
    GstElement              *pureelement;
    GParamSpec             **properties, *property;
    GValue                   value  = G_VALUE_INIT;
    GValue                   pvalue = G_VALUE_INIT;
    guint                    i, number_of_properties;
    GstElementFactory       *factory;

    element = gst_device_create_element(device, nullptr);

    if (!element)
        return nullptr;

    factory = gst_element_get_factory(element);
    if (!factory) {
        gst_object_unref(element);
        return nullptr;
    }

    if (!gst_plugin_feature_get_name(factory)) {
        gst_object_unref(element);
        return nullptr;
    }

    launch_line = g_string_new(gst_plugin_feature_get_name(factory));

    pureelement = gst_element_factory_create(factory, nullptr);

    /* get paramspecs and show non-default properties */
    properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(element), &number_of_properties);
    if (properties) {
        for (i = 0; i < number_of_properties; i++) {
            gint     j;
            gboolean ignore = FALSE;
            property        = properties[i];

            /* skip some properties */
            if ((property->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE)
                continue;

            for (j = 0; ignored_propnames[j]; j++)
                if (!g_strcmp0(ignored_propnames[j], property->name))
                    ignore = TRUE;

            if (ignore)
                continue;

            /* Can't use _param_value_defaults () because sub-classes modify the
             * values already.
             */

            g_value_init(&value, property->value_type);
            g_value_init(&pvalue, property->value_type);
            g_object_get_property(G_OBJECT(element), property->name, &value);
            g_object_get_property(G_OBJECT(pureelement), property->name, &pvalue);
            if (gst_value_compare(&value, &pvalue) != GST_VALUE_EQUAL) {
                gchar *valuestr = gst_value_serialize(&value);

                if (!valuestr) {
                    GST_WARNING("Could not serialize property %s:%s", GST_OBJECT_NAME(element), property->name);
                    g_free(valuestr);
                    goto next;
                }

                g_string_append_printf(launch_line, " %s=%s", property->name, valuestr);
                g_free(valuestr);
            }

        next:
            g_value_unset(&value);
            g_value_unset(&pvalue);
        }
        g_free(properties);
    }

    gst_object_unref(element);
    gst_object_unref(pureelement);

    return g_string_free(launch_line, FALSE);
}

class GlibTimer {
    GMainContext         *ctx;
    guint                 timer = 0;
    guint                 interval;
    std::function<bool()> callback; // return true to continue, false to stop
    bool                  stopRequested    = false;
    bool                  restartRequested = false;
    bool                  inHandler        = false;

public:
    GlibTimer(GMainContext *ctx, guint interval, std::function<bool()> callback) :
        ctx(ctx), interval(interval), callback(callback)
    {
    }

    ~GlibTimer() { stop(); }

    inline bool isActive() const { return !!timer; }

    void start()
    {
        stopRequested = false;
        if (timer) {
            stop();
            if (inHandler) {
                restartRequested = true;
                return;
            }
        }
        // qDebug("starting timer");
        struct TimerCallback {
            static gboolean call(gpointer data)
            {
                auto priv       = reinterpret_cast<GlibTimer *>(data);
                priv->inHandler = true;
                auto ret        = priv->callback();
                if (!ret) {
                    priv->timer = 0;
                }
                priv->inHandler        = false;
                auto restartRequested  = priv->restartRequested;
                auto stopRequested     = priv->stopRequested;
                priv->restartRequested = false;
                priv->stopRequested    = false;
                return restartRequested ? TRUE : stopRequested ? FALSE : ret;
            }
        };
        timer = g_timeout_add(interval, &TimerCallback::call, this);
    }

    void stop()
    {
        // qDebug("stopping timer");
        restartRequested = false;
        if (!timer) {
            return;
        }
        if (inHandler) {
            stopRequested = true;
            return;
        }
        auto source = g_main_context_find_source_by_id(ctx, timer);
        if (source) {
            g_source_destroy(source);
            g_source_unref(source);
        }
        timer = 0;
    }
};

class DeviceMonitor::Private {
public:
    DeviceMonitor           *q;
    GstMainLoop             *mainLoop;
    GstDeviceMonitor        *_monitor = nullptr;
    QMap<QString, GstDevice> _monitorDevices;
    QMap<QString, GstDevice> _platformDevices;
    PlatformDeviceMonitor   *_platform = nullptr;
    GlibTimer                timer;

    QMutex   devListMutex;
    QThread *qtThread;
    bool     started = false;

    bool videoSrcFirst  = true;
    bool audioSrcFirst  = true;
    bool audioSinkFirst = true;
    bool hasUpdates     = false;

    explicit Private(DeviceMonitor *q, GstMainLoop *mainLoop) :
        q(q), mainLoop(mainLoop), timer(mainLoop->mainContext(), 50, [this]() { return triggerUpdated(); }),
        qtThread(q->thread())
    {
    }

    ~Private()
    {
        delete _platform;
        gst_device_monitor_stop(_monitor);
        g_object_unref(_monitor);
    }

    static GstDevice gstDevConvert(::GstDevice *gdev)
    {
        PsiMedia::GstDevice d;

        gchar *ll = get_launch_line(gdev);
        if (ll) {
            auto e = gst_parse_launch(ll, nullptr);
            if (e) {
                d.id = QString::fromUtf8(ll);
                gst_object_unref(e);
            }
            g_free(ll);
            if (d.id.isEmpty() || d.id.endsWith(QLatin1String(".monitor"))) {
                d.id.clear();
                return d;
            }
        }

        gchar *name = gst_device_get_display_name(gdev);
        d.name      = QString::fromUtf8(name);
        g_free(name);

        if (gst_device_has_classes(gdev, "Audio/Source")) {
            d.type = PDevice::AudioIn;
        }

        if (gst_device_has_classes(gdev, "Audio/Sink")) {
            d.type = PDevice::AudioOut;
        }

        if (gst_device_has_classes(gdev, "Video/Source")) {
            d.type = PDevice::VideoIn;

            auto caps = gst_device_get_caps(gdev);
            for (guint i = 0; i < gst_caps_get_size(caps); i++) {
                auto                    structure = gst_caps_get_structure(caps, i);
                auto                    mime_type = gst_structure_get_name(structure);
                PsiMedia::PDevice::Caps mediaCaps;
                mediaCaps.mime = QString::fromLatin1(mime_type);
                if (gst_structure_get_int(structure, "width", &mediaCaps.video.width)
                    && gst_structure_get_int(structure, "height", &mediaCaps.video.height)
                    && gst_structure_get_fraction(structure, "framerate", &mediaCaps.video.framerate_numerator,
                                                  &mediaCaps.video.framerate_denominator)) {
                    d.caps.append(mediaCaps);
                }
            }
        }

        return d;
    }

    static gboolean onChangeGstCB(GstBus *bus, GstMessage *message, gpointer user_data)
    {
        Q_UNUSED(bus)
        auto                monObj = reinterpret_cast<DeviceMonitor::Private *>(user_data);
        PsiMedia::GstDevice d;
        ::GstDevice        *device;

        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_DEVICE_ADDED:
            gst_message_parse_device_added(message, &device);
            d = gstDevConvert(device);
            gst_object_unref(device);
            if (!d.id.isEmpty())
                monObj->onDeviceAdded(d);
            break;
        case GST_MESSAGE_DEVICE_REMOVED:
            gst_message_parse_device_removed(message, &device);
            d = gstDevConvert(device);
            gst_object_unref(device);
            if (!d.id.isEmpty())
                monObj->onDeviceRemoved(d);
            break;
#if 0
        case GST_MESSAGE_DEVICE_CHANGED: {
            gst_message_parse_device_changed(message, &device, nullptr);
            d = gstDevConvert(device);
            gst_object_unref(device);
            if (!d.id.isEmpty())
                monObj->onDeviceChanged(d);
            break;

        }
#endif
        default:
            break;
        }

        return TRUE;
    }

    void onDeviceAdded(GstDevice dev)
    {
        QMutexLocker locker(&devListMutex);
        _platformDevices.remove(dev.id);
        if (_monitorDevices.contains(dev.id)) {
            qWarning("Double added of device %s (%s)", qUtf8Printable(dev.name), qUtf8Printable(dev.id));
        } else {
            switch (dev.type) {
            case PDevice::AudioIn:
                dev.isDefault = audioSrcFirst;
                audioSrcFirst = false;
                break;
            case PDevice::AudioOut:
                dev.isDefault  = audioSinkFirst;
                audioSinkFirst = false;
                break;
            case PDevice::VideoIn:
                dev.isDefault = videoSrcFirst;
                videoSrcFirst = false;
                break;
            }
            _monitorDevices.insert(dev.id, dev);
            qDebug("added dev: %s (%s)", qUtf8Printable(dev.name), qUtf8Printable(dev.id));
            startUpdatedTimer();
        }
    }

    void onDeviceRemoved(const GstDevice &dev)
    {
        QMutexLocker locker(&devListMutex);
        if (_monitorDevices.remove(dev.id) || _platformDevices.remove(dev.id)) {
            qDebug("removed dev: %s (%s)", qUtf8Printable(dev.name), qUtf8Printable(dev.id));
            startUpdatedTimer();
        } else {
            qWarning("Double remove of device %s (%s)", qUtf8Printable(dev.name), qUtf8Printable(dev.id));
        }
    }

    void onDeviceChanged(const GstDevice &dev)
    {
        QMutexLocker locker(&devListMutex);
        auto         it = _monitorDevices.find(dev.id);
        if (it != _monitorDevices.end() || (it = _platformDevices.find(dev.id)) != _platformDevices.end()) {
            qDebug("Changed device '%s'", qUtf8Printable(dev.id));
            it->updateFrom(dev);
            startUpdatedTimer();
            return;
        }
        qDebug("Changed unknown previously device '%s'. Try to add it", qUtf8Printable(dev.id));
        onDeviceAdded(dev);
        return;
    }

    bool triggerUpdated()
    {
        if (hasUpdates) {
            hasUpdates = false;
        } else {
            return false;
        }
        qDebug("emitting devices updated");
        // the current thread doesn't have full working event loop
        // so we need to throw event from a valid thread. We had one at moment of
        // DeviceMonitor creation and preserved it in qtThread
        qtThread->metaObject()->invokeMethod(
            qtThread,
            [this, q = QPointer<DeviceMonitor>(q)]() {
                if (q) // race condition is still possible though...
                    emit q->updated();
            },
            Qt::QueuedConnection);
        return true;
    }

    inline void startUpdatedTimer()
    {
        hasUpdates = true;
        if (!timer.isActive()) {
            timer.start();
        }
    }

    void start()
    {
        if (started)
            return;
        started = true;

        qRegisterMetaType<GstDevice>("GstDevice");

        // auto context = mainLoop->mainContext();
        _platform = new PlatformDeviceMonitor;
        _monitor  = gst_device_monitor_new();

        GstBus *bus = gst_device_monitor_get_bus(_monitor);
        gst_bus_add_watch(bus, Private::onChangeGstCB, this);

        // GSource *source = gst_bus_create_watch(bus);
        // g_source_set_callback (source, (GSourceFunc)Private::onChangeGstCB, d, nullptr);
        // g_source_attach(source, context);
        // g_source_unref(source);

        gst_object_unref(bus);

        gst_device_monitor_add_filter(_monitor, "Audio/Sink", nullptr);
        gst_device_monitor_add_filter(_monitor, "Audio/Source", nullptr);

        GstCaps *caps;
        caps = gst_caps_new_empty_simple("video/x-raw");
        gst_device_monitor_add_filter(_monitor, "Video/Source", caps);
        gst_caps_unref(caps);
        caps = gst_caps_new_empty_simple("video/h264");
        gst_device_monitor_add_filter(_monitor, "Video/Source", caps);
        gst_caps_unref(caps);
        caps = gst_caps_new_empty_simple("image/jpeg");
        gst_device_monitor_add_filter(_monitor, "Video/Source", caps);
        gst_caps_unref(caps);

        updateDevList();
        if (!gst_device_monitor_start(_monitor)) {
            qWarning("failed to start device monitor");
        }
        if (_platformDevices.size() || _monitorDevices.size()) {
            hasUpdates = true;
            triggerUpdated();
        }
    }

    void updateDevList()
    {
        QMutexLocker locker(&devListMutex);

#if GST_VERSION_MAJOR == 1 && GST_VERSION_MINOR < 18
        //  with newer versions the devices events seem replayed, so we don't need this
        _monitorDevices.clear();
        GList *devices = gst_device_monitor_get_devices(_monitor);

        if (devices != NULL) {
            while (devices != NULL) {
                ::GstDevice        *device = static_cast<::GstDevice *>(devices->data);
                PsiMedia::GstDevice pdev   = Private::gstDevConvert(device);
                if (!pdev.id.isEmpty()) {
                    _monitorDevices.insert(pdev.id, pdev);
                    qDebug("found dev: %s (%s)", qUtf8Printable(pdev.name), qUtf8Printable(pdev.id));
                }
                gst_object_unref(device);
                devices = g_list_delete_link(devices, devices);
            }
        } else {
            qDebug("No devices found!");
        }
#endif

        if (_platform) {
            _platformDevices.clear();
            auto l = _platform->getDevices();
            for (auto const &pdev : std::as_const(l)) {
                if (!_platformDevices.contains(pdev.id)) {
                    _platformDevices.insert(pdev.id, pdev);
                    qDebug("found dev: %s (%s)", qUtf8Printable(pdev.name), qUtf8Printable(pdev.id));
                }
            }
        }
    }
};

DeviceMonitor::DeviceMonitor(GstMainLoop *mainLoop) : QObject(mainLoop), d(new Private(this, mainLoop)) { }

DeviceMonitor::~DeviceMonitor() = default;

void DeviceMonitor::start() { d->start(); }

QList<GstDevice> DeviceMonitor::devices(PDevice::Type type)
{
    // Called not from gst thread. other can deadlock
    QList<GstDevice> ret;

    bool hasPulsesrc         = false;
    bool hasDefaultPulsesrc  = false;
    bool hasPulsesink        = false;
    bool hasDefaultPulsesink = false;

    auto add = [&](auto const &dev) {
        if (dev.type != type)
            return;

        ret.append(dev);
        // hack for pulsesrc
        if (type == PDevice::AudioIn && dev.id.startsWith(QLatin1String("pulsesrc"))) {
            hasPulsesrc = true;
            if (dev.id == QLatin1String("pulsesrc"))
                hasDefaultPulsesrc = true;
        }
        if (type == PDevice::AudioOut && dev.id.startsWith(QLatin1String("pulsesink"))) {
            hasPulsesink = true;
            if (dev.id == QLatin1String("pulsesink"))
                hasDefaultPulsesink = true;
        }
    };

    d->devListMutex.lock();
    for (auto const &dev : std::as_const(d->_monitorDevices)) {
        add(dev);
    }
    for (auto const &dev : std::as_const(d->_platformDevices)) {
        add(dev);
    }
    d->devListMutex.unlock();

    std::sort(ret.begin(), ret.end(), [](const GstDevice &a, const GstDevice &b) { return a.name < b.name; });
    if (hasPulsesrc && !hasDefaultPulsesrc) {
        GstDevice defalt;
        defalt.isDefault = true;
        defalt.id        = "pulsesrc";
        defalt.name      = tr("Default");
        defalt.type      = type;
        ret.prepend(defalt);
    }
    if (hasPulsesink && !hasDefaultPulsesink) {
        GstDevice defalt;
        defalt.isDefault = true;
        defalt.id        = "pulsesink";
        defalt.name      = tr("Default");
        defalt.type      = type;
        ret.prepend(defalt);
    }
    return ret;
}

GstDevice *DeviceMonitor::device(const QString &id)
{
    // to be called from gst thread
    auto it = d->_monitorDevices.find(id);
    if (it != d->_monitorDevices.end()) {
        return &it.value();
    }
    it = d->_platformDevices.find(id);
    if (it != d->_platformDevices.end()) {
        return &it.value();
    }
    return nullptr;
}

GstElement *devices_makeElement(const QString &id, PDevice::Type type, QSize *captureSize)
{
    Q_UNUSED(type);
    Q_UNUSED(captureSize);
    return gst_parse_launch(id.toLatin1().data(), nullptr);
    // TODO check if it correponds to passed type.
    // TODO drop captureSize
}
}
