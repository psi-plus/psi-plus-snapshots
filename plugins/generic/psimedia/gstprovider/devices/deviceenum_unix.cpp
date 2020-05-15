/*
 * Copyright (C) 2006-2009  Justin Karneges
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

//#include <QDir>
#include <QFile>
//#include <QFileInfo>
#include <QStringList>

#include <cerrno>
#include <fcntl.h>
//#include <sys/stat.h>
//#include <sys/types.h>
#include <unistd.h>

#ifdef Q_OS_LINUX
#include <dirent.h>
//#include <linux/videodev2.h>
//#include <sys/ioctl.h>
//#include <sys/stat.h>
#endif
#include "devices.h"

namespace PsiMedia {

#define DIR_INPUT 1
#define DIR_OUTPUT 2

// taken from netinterface_unix (changed the split to KeepEmptyParts)
static QStringList read_proc_as_lines(const char *procfile)
{
    QStringList out;

    FILE *f = fopen(procfile, "r");
    if (!f)
        return out;

    QByteArray buf;
    while (!feof(f)) {
        // max read on a proc is 4K
        QByteArray block(4096, 0);
        int        ret = int(fread(block.data(), 1, size_t(block.size()), f));
        if (ret <= 0)
            break;
        block.resize(ret);
        buf += block;
    }
    fclose(f);

    QString str = QString::fromLocal8Bit(buf);
    out         = str.split('\n', QString::KeepEmptyParts);
    return out;
}

// check scheme from portaudio
static bool check_oss(const QString &dev, bool input)
{
    int fd = open(QFile::encodeName(dev).data(), (input ? O_RDONLY : O_WRONLY) | O_NONBLOCK);
    if (fd == -1) {
        if (errno == EBUSY || errno == EAGAIN)
            return false; // device is busy
        else
            return false; // can't access
    }
    close(fd);
    return true;
}

static QList<GstDevice> get_oss_items(int type)
{
    QList<GstDevice> out;

    // sndstat detection scheme from pulseaudio
    QStringList stat;
    stat = read_proc_as_lines("/dev/sndstat");
    if (stat.isEmpty()) {
        stat = read_proc_as_lines("/proc/sndstat");
        if (stat.isEmpty()) {
            stat = read_proc_as_lines("/proc/asound/oss/sndstat");
            if (stat.isEmpty())
                return out;
        }
    }

    // sndstat processing scheme from pulseaudio
    int at;
    at = stat.indexOf("Audio devices:");
    if (at == -1) {
        at = stat.indexOf("Installed devices:");
        if (at == -1)
            return out;
    }
    ++at;

    for (; at < stat.count() && !stat[at].isEmpty(); ++at) {
        QString line = stat[at];
        int     x    = line.indexOf(": ");
        if (x == -1)
            continue;

        QString devnum  = line.mid(0, x);
        QString devname = line.mid(x + 2);

        // apparently FreeBSD ids start with pcm in front
        bool bsd = false;
        if (devnum.left(3) == "pcm") {
            bsd    = true;
            devnum = devnum.mid(3);
        }

        bool ok;
        int  num = devnum.toInt(&ok);
        if (!ok)
            continue;

        x = devname.indexOf(" (DUPLEX)");
        if (x != -1)
            devname = devname.mid(0, x);

        QStringList possible;
        // apparently FreeBSD has ".0" appended to the devices
        if (bsd)
            possible += QString("/dev/dsp%1.0").arg(num);
        else
            possible += QString("/dev/dsp%1").arg(num);

        // if we're looking for the 0 item, this might be "dsp"
        //   without a number on it
        if (num == 0 && !bsd)
            possible += "/dev/dsp";

        QString dev;
        foreach (dev, possible) {
            if (QFile::exists(dev))
                break;
        }

        if (type & DIR_INPUT && check_oss(dev, true)) {
            GstDevice i;
            i.type = PDevice::AudioIn;
            i.name = QLatin1String("oss: ") + devname;
            i.id   = QLatin1String("osssrc device=") + dev;
            out += i;
        }

        if (type & DIR_OUTPUT && check_oss(dev, false)) {
            GstDevice i;
            i.type = PDevice::AudioOut;
            i.name = QLatin1String("oss: ") + devname;
            i.id   = QLatin1String("osssink device=") + dev;
            out += i;
        }
    }

    return out;
}

// /proc/asound/devices
//   16: [0- 0]: digital audio playback
//   24: [0- 0]: digital audio capture
//    0: [0- 0]: ctl
//   33:       : timer
//   56: [1- 0]: digital audio capture
//   32: [1- 0]: ctl
//
// /proc/asound/pcm
//   00-00: ALC260 Analog : ALC260 Analog : playback 1 : capture 1
//   01-00: USB Audio : USB Audio : capture 1
class AlsaItem {
public:
    int     card, dev;
    bool    input;
    QString cardName;
    QString name;
};

static QList<GstDevice> get_alsa_items(int type)
{
#ifdef Q_OS_LINUX
    QList<GstDevice> out;

    QList<AlsaItem> items;
    QStringList     devices_lines = read_proc_as_lines("/proc/asound/devices");
    foreach (QString line, devices_lines) {
        // get the fields we care about
        QString devbracket, devtype;
        int     x = line.indexOf(": ");
        if (x == -1)
            continue;
        QString sub = line.mid(x + 2);
        x           = sub.indexOf(": ");
        if (x == -1)
            continue;
        devbracket = sub.mid(0, x);
        devtype    = sub.mid(x + 2);

        // skip all but playback and capture
        bool input;
        if (devtype == "digital audio playback")
            input = false;
        else if (devtype == "digital audio capture")
            input = true;
        else
            continue;

        // skip what isn't asked for
        if (!(type & DIR_INPUT) && input)
            continue;
        if (!(type & DIR_OUTPUT) && !input)
            continue;

        // hack off brackets
        if (devbracket[0] != '[' || devbracket[devbracket.length() - 1] != ']')
            continue;
        devbracket = devbracket.mid(1, devbracket.length() - 2);

        QString cardstr, devstr;
        x = devbracket.indexOf('-');
        if (x == -1)
            continue;
        cardstr = devbracket.mid(0, x);
        devstr  = devbracket.mid(x + 1);

        AlsaItem ai;
        bool     ok;
        ai.card = cardstr.toInt(&ok);
        if (!ok)
            continue;
        ai.dev = devstr.toInt(&ok);
        if (!ok)
            continue;
        ai.input         = input;
        QByteArray  path = QByteArray("/proc/asound/card").append(QByteArray::number(ai.card)).append("/id");
        QStringList cId  = read_proc_as_lines(path.data());
        if (cId.count() > 0)
            ai.cardName = cId.at(0);
        ai.name = QString("ALSA Card %1, Device %2").arg(ai.card).arg(ai.dev);
        items += ai;
    }

    // try to get the friendly names
    QStringList pcm_lines = read_proc_as_lines("/proc/asound/pcm");
    foreach (QString line, pcm_lines) {
        QString devnumbers, devname;
        int     x = line.indexOf(": ");
        if (x == -1)
            continue;
        devnumbers = line.mid(0, x);
        devname    = line.mid(x + 2);
        x          = devname.indexOf(" :");
        if (x != -1) {
            QString devname2 = devname.mid(x + 3);
            devname          = devname.mid(0, x);
            x                = devname2.indexOf(" :");
            if (x != -1)
                devname2 = devname2.mid(0, x);
            else
                devname2 = devname2.trimmed();
            if (!devname2.isEmpty())
                devname = devname2;
        } else
            devname = devname.trimmed();

        QString cardstr, devstr;
        x = devnumbers.indexOf('-');
        if (x == -1)
            continue;
        cardstr = devnumbers.mid(0, x);
        devstr  = devnumbers.mid(x + 1);

        bool ok;
        int  cardnum = cardstr.toInt(&ok);
        if (!ok)
            continue;
        int devnum = devstr.toInt(&ok);
        if (!ok)
            continue;

        for (int n = 0; n < items.count(); ++n) {
            AlsaItem &ai = items[n];
            if (ai.card == cardnum && ai.dev == devnum)
                ai.name = devname;
        }
    }

    // make a "default" item
    if (!items.isEmpty()) {
        GstDevice i;
        if (type == DIR_INPUT) {
            i.type = PDevice::AudioIn;
            i.id   = "alsasrc";
        } else { // DIR_OUTPUT
            i.type = PDevice::AudioOut;
            i.id   = "alsasink";
        }
        i.name = "alsa: Default";
        out += i;
    }

    for (int n = 0; n < items.count(); ++n) {
        AlsaItem &ai = items[n];

        // make an item for both hw and plughw
        GstDevice i;
        // i.type = Item::Audio;
        if (ai.input) {
            i.type = PDevice::AudioIn;
            i.id   = QLatin1String("alsasrc ");
        } else {
            i.type = PDevice::AudioOut;
            i.id   = QLatin1String("alsasink ");
        }
        i.name = QLatin1String("alsa: ") + QString("[%1] %2").arg(ai.cardName).arg(ai.name);
        i.id += QString("device=plughw:%1,%2").arg(ai.card).arg(ai.dev);
        out += i;

        // internet discussion seems to indicate that plughw is the
        //   same as hw except that it will convert audio parameters
        //   if necessary.  the decision to use hw vs plughw is a
        //   development choice, NOT a user choice.  it is generally
        //   recommended for apps to use plughw unless they have a
        //   good reason.
        //
        // so, for now we'll only offer plughw and not hw
        // i.name = ai.name + " (Direct)";
        // i.id = QString().sprintf("hw:%d,%d", ai.card, ai.dev);
        // out += i;
    }

    return out;
#else
    // return empty list if non-linux
    Q_UNUSED(type);
    return QList<Item>();
#endif
}

QList<GstDevice> PlatformDeviceMonitor::getDevices()
{
    return get_oss_items(DIR_OUTPUT) + get_oss_items(DIR_INPUT) + get_alsa_items(DIR_OUTPUT)
        + get_alsa_items(DIR_INPUT);
}

} // namespace PsiMedia
