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
#include "psiaccount.h"
#include "psicon.h"
#include "psipopup.h"
#include "xmpp/jid/jid.h"

#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
#include "psigrowlnotifier.h"
#endif

#ifdef USE_DBUS
#include "psidbusnotifier.h"
#endif


static const int defaultTimeout = 5;

struct OptionValue
{
	OptionValue(const QString& name, const QString& path, int value, int _id = 0)
		: optionName(name)
		, optionPath(path)
		, optionValue(value)
		, id(_id)
	{
	}

	QString optionName;
	QString optionPath;
	int optionValue; //value in secconds
	int id;

	bool operator==(const OptionValue& other)
	{
		return optionName == other.optionName;
	}
};

class PopupManager::Private
{

public:
	Private()
		: psi_(0)
		, lastCustomType_(PopupManager::AlertCustom)
	{
	}

	PsiCon* psi_;
	int lastCustomType_;
	QList<OptionValue> options_;
};

PopupManager::PopupManager(PsiCon *psi)
{
	d = new Private;
	d->psi_ = psi;

	QList<OptionValue> initList;
	initList << OptionValue(QObject::tr("Status"), "options.ui.notifications.passive-popups.delays.status",
			PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.delays.status").toInt()/1000, AlertOnline)
	<< OptionValue(QObject::tr("Headline"),  "options.ui.notifications.passive-popups.delays.message",
			PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.delays.message").toInt()/1000, AlertHeadline)
	<< OptionValue(QObject::tr("File"), "options.ui.notifications.passive-popups.delays.file",
			PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.delays.file").toInt()/1000, AlertFile)
	<< OptionValue(QObject::tr("Chat Message"), "options.ui.notifications.passive-popups.delays.chat",
			PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.delays.chat").toInt()/1000, AlertChat)
	<< OptionValue(QObject::tr("Groupchat Message"),  "options.ui.notifications.passive-popups.delays.gc-message",
			PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.delays.gc-message").toInt()/1000, AlertGcHighlight);

	d->options_ += initList;
}

PopupManager::~PopupManager()
{
	PsiPopup::deleteAll();
	delete d;
}

int PopupManager::registerOption(const QString& name, int initValue, const QString& path)
{	
	foreach(const OptionValue& v, d->options_) {
		if(v.optionName == name)
			return v.id;
	}

	OptionValue ov(name, path, initValue, ++d->lastCustomType_);
	d->options_.append(ov);

	return ov.id;
}

void PopupManager::unregisterOption(const QString &name)
{
	OptionValue ov(name, "", 0, 0);
	d->options_.removeAll(ov);
}

void PopupManager::setValue(const QString& name, int value)
{	
	QList<OptionValue>::iterator i = d->options_.begin();
	for(; i != d->options_.end(); ++i) {
		if((*i).optionName == name) {
			(*i).optionValue = value;
		}
	}
}

int PopupManager::value(const QString& name) const
{
	foreach(const OptionValue& v, d->options_) {
		if(v.optionName == name)
			return v.optionValue;
	}

	return defaultTimeout;
}

const QString PopupManager::optionPath(const QString& name) const
{	
	foreach(const OptionValue& v, d->options_) {
		if(v.optionName == name)
			return v.optionPath;
	}

	return QString();
}

const QStringList PopupManager::optionsNamesList() const
{
	QStringList ret;
	foreach(const OptionValue& v, d->options_)
		ret.append(v.optionName);

	return ret;
}

bool PopupManager::noPopup(PsiAccount *account) const
{
	if(account) {
		if(account->noPopup())
			return true;
	}
	else {
		Status::Type type = d->psi_->currentStatusType();
		if((type == Status::DND && PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.suppress-while-dnd").toBool())
			|| ( (type == Status::Away || type == Status::XA) &&
			     PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.suppress-while-away").toBool() ))
			{
			return true;
		}
	}
	return false;
}

void PopupManager::doPopup(PsiAccount *account, PopupType pType, const Jid &j, const Resource &r,
			   UserListItem *u, PsiEvent *e, bool checkNoPopup)
{
	if (!PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.enabled").toBool())
		return;

	if(checkNoPopup && noPopup(account))
		return;

	NotificationsType type = currentType();
	PsiPopupInterface *popup = 0;
	if(type == Default) {
		popup = new PsiPopup(this);
	}
#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
	else if(type == Growl) {
		popup = PsiGrowlNotifier::instance();
	}
#endif
#ifdef USE_DBUS
	else if(type == DBus) {
		popup = new PsiDBusNotifier(this);
	}
#endif
	if(popup) {
		popup->setDuration(timeout(pType));
		popup->popup(account, pType, j, r, u, e);
	}
}

void PopupManager::doPopup(PsiAccount *account, const Jid &j, const PsiIcon *titleIcon, const QString &titleText,
			   const QPixmap *avatar, const PsiIcon *icon, const QString &text, bool checkNoPopup, PopupType pType)
{
	if (!PsiOptions::instance()->getOption("options.ui.notifications.passive-popups.enabled").toBool())
		return;

	if(checkNoPopup && noPopup(account))
		return;

	NotificationsType type = currentType();
	PsiPopupInterface *popup = 0;
	if(type == Default) {
		popup = new PsiPopup(this);
	}
#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
	else if(type == Growl) {
		popup = PsiGrowlNotifier::instance();
	}
#endif
#ifdef USE_DBUS
	else if(type == DBus) {
		popup = new PsiDBusNotifier(this);
	}
#endif
	if(popup) {
		popup->setDuration(timeout(pType));
		popup->popup(account, pType, j, titleIcon, titleText, avatar, icon, text);
	}
}


QList< PopupManager::NotificationsType > PopupManager::availableTypes()
{
	if(availableTypes_.isEmpty()) {
		availableTypes_ << Default;
#if defined(Q_WS_MAC) && defined(HAVE_GROWL)
		if(PsiGrowlNotifier::isAvailable())
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

int PopupManager::timeout(PopupType type) const
{
	if(type == AlertMessage || type == AlertAvCall)
		type = AlertChat;
	else if(type == AlertOffline || type == AlertStatusChange || type == AlertNone || type == AlertComposing)
		type = AlertOnline;

	foreach(const OptionValue& v, d->options_)
		if(v.id == type)
			return v.optionValue*1000;

	return defaultTimeout*1000;
}

QList< PopupManager::NotificationsType > PopupManager::availableTypes_ = QList< NotificationsType >();
