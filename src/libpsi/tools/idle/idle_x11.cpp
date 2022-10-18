/*
 * idle_x11.cpp - detect desktop idle time
 * Copyright (C) 2003  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "idle.h"

#if !defined(HAVE_XSS) && !defined(USE_DBUS)

IdlePlatform::IdlePlatform() { d = nullptr; }
IdlePlatform::~IdlePlatform() { }
bool IdlePlatform::init() { return false; }
int  IdlePlatform::secondsIdle() { return 0; }

#elif defined(USE_DBUS) && !defined(HAVE_X11) && !defined(LIMIT_X11_USAGE)

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>

// Screen Saver dbus services
static const QLatin1String COMMON_SS_SERV("org.freedesktop.ScreenSaver");
static const QLatin1String COMMON_SS_PATH("/ScreenSaver");
static const QLatin1String KDE_SS_SERV("org.kde.screensaver");
static const QLatin1String GNOME_SS_SERV("org.gnome.Mutter.IdleMonitor");
static const QLatin1String GNOME_SS_PATH("/org/gnome/Mutter/IdleMonitor/Core");
// Screen saver functions
static const QLatin1String GNOME_SS_F("GetIdletime");
static const QLatin1String COMMON_SS_F("GetSessionIdleTime");

class IdlePlatform::Private {
public:
    Private() { }
    QString getServicesAvailable() const
    {
        const auto        services     = QDBusConnection::sessionBus().interface()->registeredServiceNames().value();
        const QStringList idleServices = { COMMON_SS_SERV, KDE_SS_SERV, GNOME_SS_SERV };
        // find first available dbus-service
        for (const auto &service : idleServices) {
            if (services.contains(service)) {
                return service;
            }
        }
        return QString();
    }
    int sendDBusCall() const
    {
        const auto serviceName = getServicesAvailable();
        if (!serviceName.isEmpty()) {
            // KDE and freedesktop uses the same path interface and method but gnome uses other
            bool                isNotGnome = serviceName == COMMON_SS_SERV || serviceName == KDE_SS_SERV;
            const QLatin1String iface      = isNotGnome ? COMMON_SS_SERV : GNOME_SS_SERV;
            const QLatin1String path       = isNotGnome ? COMMON_SS_PATH : GNOME_SS_PATH;
            const QLatin1String method     = isNotGnome ? COMMON_SS_F : GNOME_SS_F;
            auto                interface  = QDBusInterface(serviceName, path, iface);
            if (interface.isValid()) {
                QDBusReply<uint> reply = interface.call(method);
                // probably reply value for freedesktop and kde need to be converted to seconds
                if (reply.isValid())
                    return isNotGnome ? reply.value() / 1000 : reply.value();
            }
        }
        return -1;
    }
};

IdlePlatform::IdlePlatform() { d = new Private; }
IdlePlatform::~IdlePlatform() { delete d; }
bool IdlePlatform::init() { return d->sendDBusCall() >= 0; }

int IdlePlatform::secondsIdle()
{
    const int result = d->sendDBusCall();
    return (result > 0) ? result : 0;
}

#else

#include <QApplication>
#include <QDesktopWidget>
#include <QX11Info>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/scrnsaver.h>

static XErrorHandler old_handler = 0;
extern "C" int       xerrhandler(Display *dpy, XErrorEvent *err)
{
    if (err->error_code == BadDrawable)
        return 0;

    return (*old_handler)(dpy, err);
}

class IdlePlatform::Private {
public:
    Private() { }

    XScreenSaverInfo *ss_info = nullptr;
};

IdlePlatform::IdlePlatform() { d = new Private; }

IdlePlatform::~IdlePlatform()
{
    if (d->ss_info)
        XFree(d->ss_info);
    if (old_handler) {
        XSetErrorHandler(old_handler);
        old_handler = 0;
    }
    delete d;
}

bool IdlePlatform::init()
{
    if (!QX11Info::isPlatformX11())
        return false;

    if (d->ss_info)
        return true;

    old_handler = XSetErrorHandler(xerrhandler);

    int event_base, error_base;
#if defined(HAVE_XSS) && !defined(LIMIT_X11_USAGE)
    if (XScreenSaverQueryExtension(QX11Info::display(), &event_base, &error_base)) {
        d->ss_info = XScreenSaverAllocInfo();
        return true;
    }
#endif
    return false;
}

int IdlePlatform::secondsIdle()
{
    if (!d->ss_info)
        return 0;
#if defined(HAVE_XSS)
    if (!XScreenSaverQueryInfo(QX11Info::display(), QX11Info::appRootWindow(), d->ss_info))
        return 0;
#endif
    return d->ss_info->idle / 1000;
}

#endif // ! ( defined(USE_DBUS) && !defined(HAVE_X11) && !defined(LIMIT_X11_USAGE) )
