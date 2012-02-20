/*
 * psipopup.h - the Psi passive popup class
 * Copyright (C) 2003  Michail Pishchagin
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef PSIPOPUP_H
#define PSIPOPUP_H

class FancyPopup;

#include "popupmanager.h"

class PsiPopup : public QObject
{
	Q_OBJECT
public:
	PsiPopup(PopupManager* manager, const PsiIcon *titleIcon, const QString& titleText, PsiAccount *acc, PopupManager::PopupType type);
	PsiPopup(PopupManager* manager, PopupManager::PopupType type, PsiAccount *acc);

	~PsiPopup();

	void setData(const Jid &, const Resource &, const UserListItem * = 0, const PsiEvent * = 0);
	void setData(const QPixmap *avatar, const PsiIcon *icon, const QString& text);

	void setJid(const Jid &j);

	void show();
	static void deleteAll();

	static QString title(PopupManager::PopupType type, bool *doAlertIcon, PsiIcon **icon);

private:
	QString id() const;
	FancyPopup *popup();

	PopupManager* pm_;
	class Private;
	Private *d;
	friend class Private;
};

#endif
