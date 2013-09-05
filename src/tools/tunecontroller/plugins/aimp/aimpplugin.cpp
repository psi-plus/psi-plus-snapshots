/*
 * aimpplugin.cpp
 * Copyright (C) 2012 Vitaly Tonkacheyev
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef QT_STATICPLUGIN
#define QT_STATICPLUGIN
#endif

#include <QtCore>
#include <QObject>
#include <QString>

#include "aimptunecontroller.h"
#include "tunecontrollerplugin.h"

class AIMPPlugin : public QObject, public TuneControllerPlugin
{

	Q_OBJECT

	Q_INTERFACES(TuneControllerPlugin)

public:
	virtual QString name();
	virtual TuneController* createController();
};

#ifndef HAVE_QT5
Q_EXPORT_PLUGIN2(aimpplugin, AIMPPlugin);
#endif

QString AIMPPlugin::name()
{
	return "AIMP3";
}

TuneController* AIMPPlugin::createController()
{
    return new AimpTuneController();
}

#include "aimpplugin.moc"
