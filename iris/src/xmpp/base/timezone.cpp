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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <QtGlobal>

#if QT_VERSION < QT_VERSION_CHECK(5, 2, 0)
#include <QByteArray>
#include <QTime>
#ifdef Q_OS_UNIX
#include <time.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#else
#include <QTimeZone>
#endif

#include "timezone.h"

#if QT_VERSION < QT_VERSION_CHECK(5,2,0)
static bool inited = false;
static int timezone_offset_;
static QString timezone_str_;

static void init()
{
#if defined(Q_OS_UNIX)
	time_t x;
	time(&x);
	char str[256];
	char fmt[32];
	int size;
	strcpy(fmt, "%z");
	size = strftime(str, 256, fmt, localtime(&x));
	if(size && strncmp(fmt, str, size)) {
		timezone_offset_ = QByteArray::fromRawData(str + 1, 2).toInt() * 60 + QByteArray::fromRawData(str + 3, 2).toInt();
		if(str[0] == '-')
			timezone_offset_ = -timezone_offset_;
	}
	strcpy(fmt, "%Z");
	strftime(str, 256, fmt, localtime(&x));
	if(strcmp(fmt, str))
		timezone_str_ = str;

#elif defined(Q_OS_WIN)
	TIME_ZONE_INFORMATION i;
	memset(&i, 0, sizeof(i));
	bool inDST = (GetTimeZoneInformation(&i) == TIME_ZONE_ID_DAYLIGHT);
	int bias = i.Bias;
	if(inDST)
		bias += i.DaylightBias;
	timezone_offset_ = -bias;
	timezone_str_ = "";
	for(int n = 0; n < 32; ++n) {
		int w = inDST ? i.DaylightName[n] : i.StandardName[n];
		if(w == 0)
			break;
		timezone_str_ += QChar(w);
	}

#else
	qWarning("Failed to properly init timezone data. Use UTC offset instead");
	inited = true;
	timezone_offset_ = 0;
	timezone_str_ = QLatin1String("N/A");
#endif
}
#endif

int TimeZone::offsetFromUtc()
{
#if QT_VERSION < QT_VERSION_CHECK(5,2,0)
	if (!inited) {
		init();
	}
	return timezone_offset_;
#else
	return QTimeZone::systemTimeZone().offsetFromUtc(QDateTime::currentDateTime());
#endif
}

QString TimeZone::abbreviation()
{
#if QT_VERSION < QT_VERSION_CHECK(5,2,0)
	return timezone_str_;
#else
	return QTimeZone::systemTimeZone().abbreviation(QDateTime::currentDateTime());
#endif
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
