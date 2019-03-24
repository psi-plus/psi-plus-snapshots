/*
 * Copyright (C) 2006  Justin Karneges
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
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef IRISNETGLOBAL_H
#define IRISNETGLOBAL_H

#include <QtCore>
#include <QtNetwork>
#include "irisnetexport.h"

namespace XMPP {

// set the directories for plugins.  call before doing anything else.
IRISNET_EXPORT void irisNetSetPluginPaths(const QStringList &paths);

// free any shared data and plugins.
// note: this is automatically called when qapp shuts down.
IRISNET_EXPORT void irisNetCleanup();

}

#endif
