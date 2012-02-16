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

#include <QObject>

class QPixmap;
class PsiCon;
class PsiAccount;
class UserListItem;
class FancyPopup;
class PsiIcon;
class PsiEvent;
namespace XMPP {
	class Jid;
	class Resource;
}
using namespace XMPP;


class PsiPopup : public QObject
{
	Q_OBJECT
public:
	enum PopupType {
		AlertNone = 0,

		AlertOnline,
		AlertOffline,
		AlertStatusChange,

		AlertMessage,
		AlertComposing,
		AlertChat,
		AlertHeadline,
		AlertFile,
		AlertAvCall,
		AlertGcHighlight
	};

	PsiPopup(const PsiIcon *titleIcon, const QString& titleText, PsiAccount *acc);
	PsiPopup(PopupType type, PsiAccount *acc);

	~PsiPopup();

	void setData(const Jid &, const Resource &, const UserListItem * = 0, const PsiEvent * = 0);
	void setData(const QPixmap *avatar, const PsiIcon *icon, const QString& text);

	void setJid(const Jid &j);

	void show();
	static void deleteAll();

	static int timeout(PopupType type);
	static QString title(PopupType type, bool *doAlertIcon, PsiIcon **icon);
	static QString clipText(const QString& text);

private:
	QString id() const;
	FancyPopup *popup();

	class Private;
	Private *d;
	friend class Private;
};

#endif
