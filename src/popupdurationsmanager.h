/*
 * popupdurationsmanager.h - XMPP Ping server
 * Copyright (C) 2011  Khryukin Evgeny
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

#ifndef POPUPDURATIONSMANAGER_H
#define POPUPDURATIONSMANAGER_H

#include <QStringList>
#include <QPair>
#include <QHash>

class PopupDurationsManager
{
public:
	PopupDurationsManager();
	~PopupDurationsManager() {};

	void registerOption(const QString& name, int initValue = 5, const QString& path = QString());
	void unregisterOption(const QString& name);
	void setValue(const QString& name, int value);
	int value(const QString& name) const;
	const QString optionPath(const QString& name) const;
	//const QStringList otionsPathList() const;
	const QStringList optionsNamesList() const;
	//void saveOptions() const;

private:
	typedef QPair<QString, int> OptionValue;
	QHash<QString, OptionValue> options_; // unsorted list
	QStringList optionsNames_; // list sorted by time
};

#endif
