/*
 * Copyright (C) 2006-2009  Justin Karneges
 * Copyright (C) 2006-2009  Remko Troncon
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

#include "deviceenum.h"

#include <CoreAudio/CoreAudio.h>

namespace DeviceEnum {

#define DIR_INPUT 1
#define DIR_OUTPUT 2

static int find_by_id(const QList<Item> &list, int id)
{
    for (int n = 0; n < list.count(); ++n) {
        if (list[n].id.toInt() == id)
            return n;
    }
    return -1;
}

static QList<Item> get_audio_items(int type)
{
    QList<Item> out;

    int    nb_devices = 0;
    UInt32 size       = 0;
    if (AudioHardwareGetPropertyInfo(kAudioHardwarePropertyDevices, &size, NULL) != 0)
        return out;
    nb_devices = size / sizeof(AudioDeviceID);

    // Get the devices
    AudioDeviceID devices[nb_devices];
    AudioHardwareGetProperty(kAudioHardwarePropertyDevices, &size, devices);
    for (int i = 0; i < nb_devices; i++) {
        // Get the device name
        char name[1024];
        size = sizeof(name);
        if (AudioDeviceGetProperty(devices[i], 0, 0, kAudioDevicePropertyDeviceName, &size, name) != 0)
            continue;
        QString qname = QString::fromUtf8(name);

        // Query the input streams
        if (AudioDeviceGetPropertyInfo(devices[i], 0, 1, kAudioDevicePropertyStreams, &size, NULL) != 0)
            continue;
        bool input = (size > 0);

        // Query the output streams
        if (AudioDeviceGetPropertyInfo(devices[i], 0, 0, kAudioDevicePropertyStreams, &size, NULL) != 0)
            continue;
        bool output = (size > 0);

        int dev_int = devices[i];

        if (type & DIR_INPUT && input) {
            Item i;
            i.type   = Item::Audio;
            i.dir    = Item::Input;
            i.name   = qname;
            i.driver = "osxaudio";
            i.id     = QString::number(dev_int);
            out += i;
        }

        if (type & DIR_OUTPUT && output) {
            Item i;
            i.type   = Item::Audio;
            i.dir    = Item::Output;
            i.name   = qname;
            i.driver = "osxaudio";
            i.id     = QString::number(dev_int);
            out += i;
        }
    }

    // do default output first, then input, so that if both are found, input
    //   will end up at the top.  not that it really matters.

    // Get the default output device
    if (type & DIR_OUTPUT) {
        size                         = sizeof(AudioDeviceID);
        AudioDeviceID default_output = kAudioDeviceUnknown;
        if (AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice, &size, &default_output) == 0) {
            int at = find_by_id(out, default_output);
            if (at != -1)
                out.move(at, 0);
        }
    }

    // Get the default input device
    if (type & DIR_INPUT) {
        size                        = sizeof(AudioDeviceID);
        AudioDeviceID default_input = kAudioDeviceUnknown;
        if (AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice, &size, &default_input) == 0) {
            int at = find_by_id(out, default_input);
            if (at != -1)
                out.move(at, 0);
        }
    }

    return out;
}

QList<Item> audioOutputItems(const QString &driver)
{
    Q_UNUSED(driver);
    return get_audio_items(DIR_OUTPUT);
}

QList<Item> audioInputItems(const QString &driver)
{
    Q_UNUSED(driver);
    return get_audio_items(DIR_INPUT);
}

QList<Item> videoInputItems(const QString &driver)
{
    Q_UNUSED(driver);

    QList<Item> out;

    // hardcode a default input device
    Item i;
    i.type   = Item::Video;
    i.dir    = Item::Input;
    i.name   = "Default";
    i.driver = "osxvideo";
    i.id     = QString(); // unspecified
    out += i;

    return out;
}

}
