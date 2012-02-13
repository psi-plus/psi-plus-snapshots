/*
 * popupdurationsmanager.cpp - XMPP Ping server
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

#include "popupdurationsmanager.h"

PopupDurationsManager::PopupDurationsManager()
{
}

void PopupDurationsManager::registerOption(const QString& name, int initValue, const QString& path)
{
	if(optionsNames_.contains(name)) {
		return;
	}

	optionsNames_.push_back(name);
	OptionValue ov(path, initValue);
	options_[name] = ov;
}

void PopupDurationsManager::unregisterOption(const QString &name)
{
	optionsNames_.removeAll(name);
	options_.remove(name);
}

void PopupDurationsManager::setValue(const QString& name, int value)
{
	if(!optionsNames_.contains(name)) {
		return;
	}

	OptionValue ov = options_.value(name);
	ov.second = value;
	options_[name] = ov;
}

int PopupDurationsManager::value(const QString& name) const
{
	if(!optionsNames_.contains(name)) {
		return 5;
	}

	OptionValue ov = options_.value(name);
	return ov.second;
}

const QString PopupDurationsManager::optionPath(const QString& name) const
{
	if(!optionsNames_.contains(name)) {
		return QString();
	}

	OptionValue ov = options_.value(name);
	return ov.first;
}

//const QStringList PopupDurationsManager::otionsPathList() const
//{
//	QStringList l;
//	foreach(const OptionValue& ov, options_.values()) {
//		l.push_back(ov.first);
//	}
//	return l;
//}

const QStringList PopupDurationsManager::optionsNamesList() const
{
	return optionsNames_;
}
//
//void PopupDurationsManager::saveOptions() const
//{
//
//}
