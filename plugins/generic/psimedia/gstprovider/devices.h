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

#ifndef PSIMEDIA_DEVICES_H
#define PSIMEDIA_DEVICES_H

#include "psimediaprovider.h"

#include <QList>
#include <QString>
#include <gst/gstelement.h>

#include <memory>

class QSize;

namespace PsiMedia {

class GstMainLoop;

class GstDevice {
public:
    PDevice::Type        type;
    bool                 isDefault = false; // TODO assign true somewhere
    QString              name;
    QString              id;
    QList<PDevice::Caps> caps;

    void updateFrom(const GstDevice &dev)
    {
        name      = dev.name;
        isDefault = dev.isDefault;
        caps      = dev.caps;
    }

    PDevice toPDevice() const
    {
        PDevice out;
        out.type      = type;
        out.name      = name;
        out.id        = id;
        out.isDefault = isDefault;
        out.caps      = caps;
        return out;
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
    std::unique_ptr<Private> d;

signals:
    void updated();

public:
    explicit DeviceMonitor(GstMainLoop *mainLoop);
    ~DeviceMonitor() override;

    void             start();
    QList<GstDevice> devices(PDevice::Type type);
    GstDevice       *device(const QString &id);
};

GstElement *devices_makeElement(const QString &id, PDevice::Type type, QSize *captureSize = nullptr);

}

Q_DECLARE_METATYPE(PsiMedia::GstDevice)

#endif // PSIMEDIA_DEVICES_H
