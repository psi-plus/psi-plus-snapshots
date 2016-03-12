/*
 * Copyright (C) 2016  Remko Troncon, Rion
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

#ifndef XMPP_CAPS_H
#define XMPP_CAPS_H

#include <QPointer>

#include "xmpp_features.h"
#include "xmpp_discoitem.h"
#include "xmpp_status.h"


namespace XMPP {

class CapsInfo
{
public:
	inline CapsInfo() {}
	inline CapsInfo(const XMPP::DiscoItem &disco, const QDateTime &lastSeen = QDateTime()) :
		_lastSeen(lastSeen.isNull()? QDateTime::currentDateTime() : lastSeen),
		_disco(disco) {}
	inline bool isValid() const { return _lastSeen.isValid(); }
	inline const QDateTime &lastSeen() const { return _lastSeen; }
	inline const XMPP::DiscoItem &disco() const { return _disco; }
	QDomElement toXml(QDomDocument *doc) const;
	static CapsInfo fromXml(const QDomElement &ci);

private:
	QDateTime _lastSeen;
	XMPP::DiscoItem _disco;
};


class CapsRegistry : public QObject
{
	Q_OBJECT

public:
	CapsRegistry(QObject *parent = 0);

	static CapsRegistry* instance();
	static void setInstance(CapsRegistry*instance);

	void registerCaps(const CapsSpec&, const XMPP::DiscoItem &item);
	bool isRegistered(const QString &) const;
	DiscoItem disco(const QString&) const;

signals:
	void registered(const XMPP::CapsSpec&);

public slots:
	void load();
	void save();

protected:
	virtual void saveData(const QByteArray &data); // reimplmenet these two functions
	virtual QByteArray loadData();                 // to have permanent cache

private:
	static CapsRegistry *instance_;
	QHash<QString,CapsInfo> capsInfo_;
};


class CapsManager : public QObject
{
	Q_OBJECT

public:
	CapsManager(Client *client);
	~CapsManager();

	bool isEnabled();
	void setEnabled(bool);

	void updateCaps(const Jid& jid, const CapsSpec& caps);
	void disableCaps(const Jid& jid);
	bool capsEnabled(const Jid& jid) const;
	XMPP::DiscoItem disco(const Jid &jid) const;
	XMPP::Features features(const Jid& jid) const;
	QString clientName(const Jid& jid) const;
	QString clientVersion(const Jid& jid) const;
	QString osVersion(const Jid& jid) const;
	CapsSpec capsSpec(const Jid &jid) const;

signals:
	/**
	 * This signal is emitted when the feature list of a given JID have changed.
	 */
	void capsChanged(const Jid& jid);

protected slots:
	void discoFinished();
	void capsRegistered(const CapsSpec&);

private:
	Client *client_;
	bool isEnabled_;
	QMap<QString,CapsSpec> capsSpecs_;
	QMap<QString,QList<QString> > capsJids_;
};

} // namespace XMPP

#endif // CAPS_H
