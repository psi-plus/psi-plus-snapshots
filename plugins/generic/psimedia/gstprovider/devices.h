/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
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

#ifndef DEVICES_H
#define DEVICES_H

#include "psimediaprovider.h"
#include <QList>
#include <QString>
#include <gst/gstelement.h>

class QSize;

namespace PsiMedia {

class GstMainLoop;

class GstDevice {
public:
    PDevice::Type type;
    QString       name;
    bool          isDefault = false; // TODO assign true somewhere
    QString       id;

    void updateFrom(const GstDevice &dev)
    {
        name      = dev.name;
        isDefault = dev.isDefault;
    }
};

class PlatformDeviceMonitor {
public:
    QList<GstDevice> getDevices();
};

class DeviceMonitor : public QObject {
    Q_OBJECT

    class Private;
    // friend class Private;
    Private *d;

    void updateDevList();

private slots:
    void onDeviceAdded(GstDevice dev);
    void onDeviceRemoved(const GstDevice &dev);
    void onDeviceChanged(const GstDevice &dev);

signals:
    void updated();

public:
    explicit DeviceMonitor(GstMainLoop *mainLoop);
    ~DeviceMonitor() override;

    void             start();
    QList<GstDevice> devices(PDevice::Type type);
};

GstElement *devices_makeElement(const QString &id, PDevice::Type type, QSize *captureSize = nullptr);

}

Q_DECLARE_METATYPE(PsiMedia::GstDevice)

#endif
