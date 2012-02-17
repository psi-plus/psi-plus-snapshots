/*
 * popupmanager.h
 * Copyright (C) 2011-2012  Khryukin Evgeny
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

#ifndef POPUPMANAGER_H
#define POPUPMANAGER_H

#include "psipopup.h"

#include <QStringList>
#include <QPair>
#include <QHash>

class PsiAccount;
class PsiCon;

class PopupManager
{
public:
	PopupManager(PsiCon* psi);
	~PopupManager() {}

	enum NotificationsType {
		Default = 0,
		Growl = 1,
		DBus = 2
	};

	void registerOption(const QString& name, int initValue = 5, const QString& path = QString());
	void unregisterOption(const QString& name);
	void setValue(const QString& name, int value);
	int value(const QString& name) const;
	const QString optionPath(const QString& name) const;
	const QStringList optionsNamesList() const;

	static QList< NotificationsType > availableTypes();
	static NotificationsType currentType();
	static QString nameByType(NotificationsType type);

	void doPopup(PsiAccount* account, PsiPopup::PopupType type, const Jid& j, const Resource& r,
			    UserListItem* u = 0, PsiEvent* e = 0, bool checkNoPopup = true);
	void doPopup(PsiAccount *account, const Jid &j, const PsiIcon *titleIcon, const QString& titleText,
			    const QPixmap *avatar, const PsiIcon *icon, const QString& text, bool checkNoPopup = true);

private:
	bool noPopup(PsiAccount *account) const;

private:
	PsiCon* psi_;
	typedef QPair<QString, int> OptionValue;
	QHash<QString, OptionValue> options_;

	static QList< NotificationsType > availableTypes_;
};

#endif
