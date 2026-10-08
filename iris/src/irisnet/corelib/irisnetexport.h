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

#ifndef IRISNETEXPORT_H
#define IRISNETEXPORT_H

#include <iris/iris_export.h>

#ifdef IRISNET_STATIC
#define IRISNET_EXPORT
#define IRISNET_NO_EXPORT
#elif defined(IRISNET_MAKEDLL)
#define IRISNET_EXPORT Q_DECL_EXPORT
#define IRISNET_NO_EXPORT Q_DECL_HIDDEN
#else
// irisnet is part of the Iris library, so its public API follows the same
// shared/static and producer/consumer export policy as the XMPP API.
#define IRISNET_EXPORT IRIS_EXPORT
#define IRISNET_NO_EXPORT IRIS_NO_EXPORT
#endif

#endif // IRISNETEXPORT_H
