/*
 * psidbusnotifier.h: Psi's interface to org.freedesktop.Notify
 * Copyright (C) 2012  Khryukin Evgeny
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * You can also redistribute and/or modify this program under the
 * terms of the Psi License, specified in the accompanied COPYING
 * file, as published by the Psi Project; either dated January 1st,
 * 2005, or (at your option) any later version.
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

#ifndef PSIDBUSNOTIFIER_H
#define PSIDBUSNOTIFIER_H

#include "popupmanager.h"
#include "xmpp/jid/jid.h"

class QDBusPendingCallWatcher;
class QTimer;

using namespace XMPP;

class PsiDBusNotifier : public QObject
{
	Q_OBJECT

public:
	PsiDBusNotifier(PopupManager* manager);
	~PsiDBusNotifier();
	static bool isAvailable();
	static QStringList capabilities();
	void popup(PsiAccount* account, PopupManager::PopupType type, const Jid& j, const Resource& r, const UserListItem* = 0, PsiEvent* = 0);
	void popup(PsiAccount *account, const Jid &j, const PsiIcon *titleIcon, const QString& titleText,
				    const QPixmap *avatar, const PsiIcon *icon, const QString& text, PopupManager::PopupType type);

private slots:
	void popupClosed(uint id, uint reason);
	void eventDestroyed();
	void asyncCallFinished(QDBusPendingCallWatcher*);
	void readyToDie();

private:
	static bool checkServer();

private:
	PopupManager* pm_;
	Jid jid_;
	uint id_;
	PsiAccount *account_;
	PsiEvent *event_;
	QTimer *lifeTimer_;
	static QStringList caps_;
};

#endif
