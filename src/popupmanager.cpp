/*
 * popupmanager.cpp
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

#include "popupmanager.h"

#include "psioptions.h"

#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
#include "psigrowlnotifier.h"
#endif

#ifdef USE_DBUS
#include "psidbusnotifier.h"
#endif


static const int defaultTimeout = 5;

PopupManager::PopupManager()
{
}

void PopupManager::registerOption(const QString& name, int initValue, const QString& path)
{
	if(options_.keys().contains(name)) {
		return;
	}

	OptionValue ov(path, initValue);
	options_[name] = ov;
}

void PopupManager::unregisterOption(const QString &name)
{
	options_.remove(name);
}

void PopupManager::setValue(const QString& name, int value)
{
	if(!options_.keys().contains(name)) {
		return;
	}

	OptionValue ov = options_.value(name);
	ov.second = value;
	options_[name] = ov;
}

int PopupManager::value(const QString& name) const
{
	if(!options_.keys().contains(name)) {
		return defaultTimeout;
	}

	OptionValue ov = options_.value(name);
	return ov.second;
}

const QString PopupManager::optionPath(const QString& name) const
{
	if(!options_.keys().contains(name)) {
		return QString();
	}

	OptionValue ov = options_.value(name);
	return ov.first;
}

const QStringList PopupManager::optionsNamesList() const
{
	return options_.keys();
}

void PopupManager::doPopup(PsiAccount *account, PsiPopup::PopupType pType, const Jid &j, const Resource &r, UserListItem *u, PsiEvent *e)
{
	if ( !PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.enabled").toBool() )
		return;

	NotificationsType type = currentType();
	if(type == Default) {
		PsiPopup *popup = new PsiPopup(pType, account);
		popup->setData(j, r, u, e);
	}
#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
	else if(type == Growl) {
		PsiGrowlNotifier::instance()->popup(account, pType, j, r, u, e);
	}
#endif
#ifdef USE_DBUS
	else if(type == DBus) {
		PsiDBusNotifier *db = new PsiDBusNotifier;
		db->popup(account, pType, j, r, u, e);
	}
#endif
}

void PopupManager::doPopup(PsiAccount *account, const Jid &j, const PsiIcon *titleIcon, const QString &titleText,
			   const QPixmap *avatar, const PsiIcon *icon, const QString &text)
{
	Q_UNUSED(avatar)
	Q_UNUSED(icon)

	if ( !PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.enabled").toBool() )
		return;

	NotificationsType type = currentType();
	if(type == Default) {
		PsiPopup *popup = new PsiPopup(titleIcon, titleText, account);
		popup->setJid(j);
		popup->setData(avatar, icon, text);
	}
#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
	else if(type == Growl) {
		PsiGrowlNotifier::instance()->popup(account, j, titleIcon, titleText, text);
	}
#endif
#ifdef USE_DBUS
	else if(type == DBus) {
		PsiDBusNotifier *db = new PsiDBusNotifier;
		db->popup(account, j, titleIcon, titleText, avatar, icon, text);
	}
#endif
}


QList< PopupManager::NotificationsType > PopupManager::availableTypes()
{
	if(availableTypes_.isEmpty()) {
		availableTypes_ << Default;
#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
		availableTypes_ << Growl;
#endif
#ifdef USE_DBUS
		if(PsiDBusNotifier::isAvailable())
			availableTypes_ << DBus;
#endif
	}
	return availableTypes_;
}

PopupManager::NotificationsType PopupManager::currentType()
{
	NotificationsType type = (NotificationsType)PsiOptions::instance()->getOption("options.ui.notifications.type").toInt();
	if(availableTypes().contains(type))
		return type;

	return Default;
}

QString PopupManager::nameByType(NotificationsType type)
{
	QString ret;
	switch(type) {
	case Default:
		ret = QObject::tr("Classic");
		break;
	case Growl:
		ret = QObject::tr("Growl");
		break;
	case DBus:
		ret = QObject::tr("DBus");
		break;
	default:
		break;
	}

	return ret;
}

QList< PopupManager::NotificationsType > PopupManager::availableTypes_ = QList< NotificationsType >();
