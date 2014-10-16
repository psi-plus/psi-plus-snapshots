/*
 * systemwatch_unix.h - Detect changes in the system state (Unix).
 * Copyright (C) 2005  Remko Troncon
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
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "systemwatch_unix.h"
#ifdef USE_DBUS
# include <QDBusConnection>
#endif

UnixSystemWatch::UnixSystemWatch()
{
#ifdef USE_DBUS
	// TODO: check which service we should listen to
	QDBusConnection conn = QDBusConnection::systemBus();
	// listen to systemd's logind
	// TODO: use delaying Inhibitor locks
	conn.connect("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "PrepareForSleep", this, SLOT(prepareForSleep(bool)));
	// listen to UPower
	conn.connect("org.freedesktop.UPower", "/org/freedesktop/UPower", "org.freedesktop.UPower", "Sleeping", this, SLOT(sleeping()));
	conn.connect("org.freedesktop.UPower", "/org/freedesktop/UPower", "org.freedesktop.UPower", "Resuming", this, SLOT(resuming()));
#endif
}

void UnixSystemWatch::prepareForSleep(bool beforeSleep)
{
	if (beforeSleep)
	{
		emit sleep();
	}
	else
	{
		emit wakeup();
	}
}

void UnixSystemWatch::sleeping()
{
	emit sleep();
}

void UnixSystemWatch::resuming()
{
	emit wakeup();
}
