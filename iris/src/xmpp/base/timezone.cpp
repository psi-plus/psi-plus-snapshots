/*
 * Copyright (C) Psi Development Team
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

#include "timezone.h"


#include <QTimeZone>
#include <QtGlobal>


int TimeZone::offsetFromUtc()
{
    return QTimeZone::systemTimeZone().offsetFromUtc(QDateTime::currentDateTime()) / 60;
}

QString TimeZone::abbreviation()
{
    return QTimeZone::systemTimeZone().abbreviation(QDateTime::currentDateTime());
}

int TimeZone::tzdToInt(const QString &tzd)
{
    int tzoSign = 1;
    if (tzd.startsWith('Z')) {
        return 0;
    } else if (tzd.startsWith('+') || tzd.startsWith('-')) {
        QTime time = QTime::fromString(tzd.mid(1), "hh:mm");
        if (time.isValid()) {
            if (tzd[0] == '-') {
                tzoSign = -1;
            }
            return tzoSign * (time.hour() * 60 + time.second());
        }
    }
    return -1; /* we don't have -1 sec offset. and usually the value is common for errors */
}

/**
 * \fn int TimeZone::timezoneOffset()
 * \brief Local timezone offset in minutes.
 */

/**
 * \fn QString TimeZone::timezoneString()
 * \brief Local timezone name.
 */
