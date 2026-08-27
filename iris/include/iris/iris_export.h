/*
 * Copyright (C) 2026  Psi Development Team
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
 */

#ifndef IRIS_EXPORT_H
#define IRIS_EXPORT_H

#include <QtCore/qglobal.h>

#if defined(IRIS_STATIC)
#define IRIS_EXPORT
#elif defined(IRIS_BUILDING_LIBRARY)
#define IRIS_EXPORT Q_DECL_EXPORT
#else
#define IRIS_EXPORT Q_DECL_IMPORT
#endif

#endif // IRIS_EXPORT_H
