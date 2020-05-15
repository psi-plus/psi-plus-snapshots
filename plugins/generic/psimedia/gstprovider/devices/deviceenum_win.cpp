/*
 * Copyright (C) 2008-2009  Justin Karneges
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

namespace PsiMedia {

QList<Item> audioOutputItems(const QString &driver)
{
    Q_UNUSED(driver);

    QList<Item> out;

    // hardcode a default output device
    Item i;
    i.type   = Item::Audio;
    i.dir    = Item::Output;
    i.name   = "Default";
    i.driver = "directsound";
    i.id     = QString(); // unspecified
    out += i;

    return out;
}

QList<Item> audioInputItems(const QString &driver)
{
    Q_UNUSED(driver);

    QList<Item> out;

    // hardcode a default input device
    Item i;
    i.type   = Item::Audio;
    i.dir    = Item::Input;
    i.name   = "Default";
    i.driver = "directsound";
    i.id     = QString(); // unspecified
    out += i;

    return out;
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
    i.driver = "winks";
    i.id     = QString(); // unspecified
    out += i;

    return out;
}

} // namespace PsiMedia
