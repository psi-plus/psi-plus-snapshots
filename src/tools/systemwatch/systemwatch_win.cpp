/*
 * systemwatch_win.cpp - Detect changes in the system state (Windows).
 * Copyright (C) 2005, 2008  James Chaldecott, Maciej Niedzielski
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "systemwatch_win.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QWidget>
#include <windows.h>

// workaround for the very old MinGW version bundled with Qt
#ifndef PBT_APMSUSPEND
#include <pbt.h>
#endif

/*
    Implementor notes:

    This class needs to get Windows messages.
    The easiest way is to get them from a top level QWidget instance.
    There was an attempt to use QApplication::winEventFilter(),
    but - as its name says - this is a filter, so all messages to
    all widgets go through it. So as a consequence, sleep() and wakeup()
    are emited many times during one event.

    Right now, there is a dummy window created just for SystemWatch.
    This may seem to be an unnecesary waste of resources, but the example
    above shows that too aggressive optimizations may hurt.
    A possible solution "in between" would be to catch events in already
    existing window (main window, probably)
    and pass them (by using ugly casting) directly to processWinEvent()
    But this would break the beauty of this tool.
*/

// -----------------------------------------------------------------------------
// WinSystemWatch
// -----------------------------------------------------------------------------

class WinSystemWatch::EventFilter : public QAbstractNativeEventFilter {
    WinSystemWatch *syswatch;

public:
    EventFilter(WinSystemWatch *parent) : syswatch(parent) { qApp->installNativeEventFilter(this); }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    virtual bool nativeEventFilter(const QByteArray &eventType, void *m, long *result) Q_DECL_OVERRIDE
#else
    bool nativeEventFilter(const QByteArray &eventType, void *m, qintptr *result) override
#endif
    {
        if (eventType == "windows_generic_MSG") {
            return syswatch->processWinEvent(static_cast<MSG *>(m), result);
        }
        return false;
    }
};

WinSystemWatch::WinSystemWatch() { d = new EventFilter(this); }

WinSystemWatch::~WinSystemWatch()
{
    delete d;
    d = 0;
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
bool WinSystemWatch::processWinEvent(MSG *m, long *result)
#else
bool WinSystemWatch::processWinEvent(MSG *m, qintptr *result)
#endif
{
    Q_UNUSED(result);
    if (WM_POWERBROADCAST == m->message) {
        switch (m->wParam) {
        case PBT_APMSUSPEND:
            emit sleep();
            break;

        case PBT_APMRESUMESUSPEND:
            emit wakeup();
            break;

        case PBT_APMRESUMECRITICAL:
            // The system previously went into SUSPEND state (suddenly)
            // without sending PBT_APMSUSPEND.  Net connections are
            // probably invalid.  Not sure what to do about this.
            // Maybe:
            emit sleep();
            emit wakeup();
            break;

        case PBT_APMQUERYSUSPEND:
            // TODO: Check if file transfers are running, and don't go
            // to sleep if there are.  To refuse to suspend, we somehow
            // need to return BROADCAST_QUERY_DENY from the actual
            // windows procedure.
            break;
        }
    } else if (WM_QUERYENDSESSION == m->message) {
        // TODO : If we allow the user to cancel suspend if they
        // are doing a file transfer, we should probably also give
        // them the chance to cancel a shutdown or log-off
    }

    return false; // Let Qt handle the right return value.
}
