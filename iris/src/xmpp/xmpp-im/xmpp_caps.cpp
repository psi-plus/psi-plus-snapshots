/*
 * capsregistry.cpp
 * Copyright (C) 2006-2016  Remko Troncon, Rion
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

/*
 * We have one global CapsRegistry to cache disco results from all application.
 * Each Client has its own CapsManager to control caps just for an Account.
 * Caps result is stored in CapsInfo class and <c> node from presence
 * goes to CapsSpec.
 */

#include <QCoreApplication>
#include <QDebug>
#include <QTextCodec>
#include <QFile>
#include <QDomElement>

#include "xmpp_features.h"
#include "xmpp_caps.h"
#include "xmpp_discoinfotask.h"
#include "xmpp_client.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {


QDomElement CapsInfo::toXml(QDomDocument *doc) const
{
	QDomElement caps = doc->createElement("info");
	caps.appendChild(textTag(doc, "atime", _lastSeen.toString(Qt::ISODate)));
	caps.appendChild(_disco.toDiscoInfoResult(doc));
	return caps;
}

CapsInfo CapsInfo::fromXml(const QDomElement &caps)
{
	QDateTime lastSeen = QDateTime::fromString(caps.firstChildElement("atime").nodeValue(), Qt::ISODate);
	DiscoItem item = DiscoItem::fromDiscoInfoResult(caps.firstChildElement("query"));
	if (item.features().isEmpty()) { // it's hardly possible if client does not support anything.
		return CapsInfo();
	}
	return CapsInfo(item, lastSeen);
}


// -----------------------------------------------------------------------------

/**
 * \class CapsRegistry
 * \brief A singleton class managing the capabilities of clients.
 */
CapsRegistry *CapsRegistry::instance_ = 0;

/**
 * \brief Default constructor.
 */
CapsRegistry::CapsRegistry(QObject *parent) :
	QObject(parent)
{
}

CapsRegistry *CapsRegistry::instance()
{
	if (!instance_) {
		instance_ = new CapsRegistry(qApp);
	}
	return instance_;
}

void CapsRegistry::setInstance(CapsRegistry *instance)
{
	instance_ = instance;
}

/**
 * \brief Convert all capabilities info to XML.
 */
void CapsRegistry::save()
{
	// Generate XML
	QDomDocument doc;
	QDomElement capabilities = doc.createElement("capabilities");
	doc.appendChild(capabilities);
	QHash<QString,CapsInfo>::ConstIterator i = capsInfo_.constBegin();
	for( ; i != capsInfo_.end(); i++) {
		QDomElement info = i.value().toXml(&doc);
		info.setAttribute("node",i.key());
		capabilities.appendChild(info);
	}

	saveData(doc.toString().toUtf8());
}

void CapsRegistry::saveData(const QByteArray &data)
{
	Q_UNUSED(data)
	return;
}

QByteArray CapsRegistry::loadData()
{
	return QByteArray();
}

/**
 * \brief Sets the file to save the capabilities info to
 */
void CapsRegistry::load()
{
	QByteArray data = loadData();
	if (data.isEmpty()) {
		return;
	}

	// Load settings
	QDomDocument doc;

	if (!doc.setContent(QString::fromUtf8(data))) {
		qWarning() << "CapsRegistry: Cannnot parse input";
		return;
	}

	QDomElement caps = doc.documentElement();
	if (caps.tagName() != "capabilities") {
		qWarning("caps.cpp: Invalid capabilities element");
		return;
	}

	// keep unseen info for last 3 month. adjust if required
	QDateTime validTime = QDateTime::currentDateTime().addMonths(-3);
	for(QDomNode n = caps.firstChild(); !n.isNull(); n = n.nextSibling()) {
		QDomElement i = n.toElement();
		if(i.isNull()) {
			qWarning("capsregistry.cpp: Null element");
			continue;
		}

		if(i.tagName() == "info") {
			QString node = i.attribute("node");
			int sep = node.indexOf('#');
			if (sep > 0 && sep + 1 < node.length()) {
				CapsInfo info = CapsInfo::fromXml(i);
				if (info.isValid() && info.lastSeen() > validTime) {
					capsInfo_[node] = CapsInfo::fromXml(i);
				}
				//qDebug() << QString("Read %1 %2").arg(node).arg(ver);
			}
			else {
				qWarning() << "capsregistry.cpp: Node" << node << "invalid";
			}
		}
		else {
			qWarning("capsregistry.cpp: Unknown element");
		}
	}
}

/**
 * \brief Registers capabilities of a client.
 */
void CapsRegistry::registerCaps(const CapsSpec& spec, const DiscoItem &item)
{
	QString dnode = spec.flatten();
	if (!isRegistered(dnode)) {
		CapsInfo info(item);
		capsInfo_[dnode] = info;
		emit registered(spec);
	}
}

/**
 * \brief Checks if capabilities have been registered.
 */
bool CapsRegistry::isRegistered(const QString& spec) const
{
	return capsInfo_.contains(spec);
}

DiscoItem CapsRegistry::disco(const QString &spec) const
{
	CapsInfo ci = capsInfo_.value(spec);
	return ci.disco();
}


/*--------------------------------------------------------------
  _____                __  __
 / ____|              |  \/  |
| |     __ _ _ __  ___| \  / | __ _ _ __   __ _  __ _  ___ _ __
| |    / _` | '_ \/ __| |\/| |/ _` | '_ \ / _` |/ _` |/ _ \ '__|
| |___| (_| | |_) \__ \ |  | | (_| | | | | (_| | (_| |  __/ |
 \_____\__,_| .__/|___/_|  |_|\__,_|_| |_|\__,_|\__, |\___|_|
            | |                                  __/ |
            |_|                                 |___/

--------------------------------------------------------------*/

/**
 * \class CapsManager
 * \brief A class managing all the capabilities of JIDs and their
 * clients.
 */

/**
 * \brief Default constructor.
 */
CapsManager::CapsManager(Client *client) :
	client_(client),
	isEnabled_(true)
{}

CapsManager::~CapsManager()
{}


/**
 * \brief Checks whether the caps manager is enabled (and does lookups).
 */
bool CapsManager::isEnabled()
{
	return isEnabled_;
}

/**
 * \brief Enables or disables the caps manager.
 */
void CapsManager::setEnabled(bool b)
{
	isEnabled_ = b;
}

/**
 * \brief Registers new incoming capabilities information of a JID.
 * If the features of the entity are unknown, discovery requests are sent to
 * retrieve the information.
 *
 * @param jid The entity's JID
 * @param node The entity's caps node
 * @param ver The entity's caps version
 * @param ext The entity's caps extensions
 */
void CapsManager::updateCaps(const Jid& jid, const CapsSpec &c)
{
	if (jid.compare(client_->jid(),false))
		return;

	QString fullNode = c.flatten();
	if (capsSpecs_[jid.full()] != c) {
		//qDebug() << QString("caps.cpp: Updating caps for %1 (node=%2,ver=%3,ext=%4)").arg(QString(jid.full()).replace('%',"%%")).arg(node).arg(ver).arg(ext);

		// Unregister from all old caps node
		capsJids_[capsSpecs_[jid.full()].flatten()].removeAll(jid.full());

		if (c.isValid()) {
			// Register with all new caps nodes
			capsSpecs_[jid.full()] = c;
			if (!capsJids_[fullNode].contains(jid.full())) {
				capsJids_[fullNode].push_back(jid.full());
			}

			emit capsChanged(jid);

			// Register new caps and check if we need to discover features
			if (isEnabled()) {
				if (!CapsRegistry::instance()->isRegistered(fullNode) && capsJids_[fullNode].count() == 1) {
					//qDebug() << QString("caps.cpp: Sending disco request to %1, node=%2").arg(QString(jid.full()).replace('%',"%%")).arg(node + "#" + s.extensions());
					JT_DiscoInfo* disco = new JT_DiscoInfo(client_->rootTask());
					connect(disco, SIGNAL(finished()), SLOT(discoFinished()));
					disco->get(jid, fullNode);
					disco->go(true);
				}
			}
		}
		else {
			// Remove all caps specifications
			qWarning() << QString("caps.cpp: Illegal caps info from %1: node=%2, ver=%3").arg(QString(jid.full()).replace('%',"%%")).arg(fullNode).arg(c.version());
			capsSpecs_.remove(jid.full());
		}
	}
	else {
		// Add to the list of jids
		capsJids_[fullNode].push_back(jid.full());
	}
}

/**
 * \brief Removes all feature information for a given JID.
 *
 * @param jid The entity's JID
 */
void CapsManager::disableCaps(const Jid& jid)
{
	//qDebug() << QString("caps.cpp: Disabling caps for %1.").arg(QString(jid.full()).replace('%',"%%"));
	if (capsEnabled(jid)) {
		QString node = capsSpecs_[jid.full()].flatten();
		if (!node.isEmpty()) {
			capsJids_[node].removeAll(jid.full());
		}
		capsSpecs_.remove(jid.full());
		emit capsChanged(jid);
	}
}

/**
 * \brief Called when a reply to disco#info request was received.
 * If the result was succesful, the resulting features are recorded in the
 * features database for the requested node, and all the affected jids are
 * put in the queue for update notification.
 */
void CapsManager::discoFinished()
{
	//qDebug() << QString("caps.cpp: Disco response from %1, node=%2").arg(QString(jid.full()).replace('%',"%%")).arg(node);
	// Update features
	JT_DiscoInfo *task = (JT_DiscoInfo *)sender();
	CapsSpec cs = capsSpecs_.value(task->jid().full());
	if (!cs.isValid()) {
		return;
	}
	if (task->item().capsHash(cs.hashAlgorithm()) == cs.version()) {
		CapsRegistry::instance()->registerCaps(cs, task->item());
	}
}

/**
 * \brief This slot is called whenever capabilities of a client were discovered.
 * All jids with the corresponding client are updated.
 */
void CapsManager::capsRegistered(const CapsSpec& cs)
{
	// Notify affected jids.
	foreach(const QString &s, capsJids_[cs.flatten()]) {
		//qDebug() << QString("caps.cpp: Notifying %1.").arg(s.replace('%',"%%"));
		emit capsChanged(s);
	}
}

/**
 * \brief Checks whether a given JID is broadcastingn its entity capabilities.
 */
bool CapsManager::capsEnabled(const Jid& jid) const
{
	return capsSpecs_.contains(jid.full());
}


/**
 * \brief Requests the list of features of a given JID.
 */
XMPP::DiscoItem CapsManager::disco(const Jid& jid) const
{
	//qDebug() << "caps.cpp: Retrieving features of " << jid.full();
	QStringList f;
	if (!capsEnabled(jid)) {
		return DiscoItem();
	}
	QString node = capsSpecs_[jid.full()].flatten();
	//qDebug() << QString("	%1").arg(CapsRegistry::instance()->features(s).list().join("\n"));
	return CapsRegistry::instance()->disco(node);
}

/**
 * \brief Requests the list of features of a given JID.
 */
XMPP::Features CapsManager::features(const Jid& jid) const
{
	return disco(jid).features();
}

/**
 * \brief Returns the client name of a given jid.
 * \param jid the jid to retrieve the client name of
 */
QString CapsManager::clientName(const Jid& jid) const
{
	if (capsEnabled(jid)) {
		CapsSpec cs = capsSpecs_[jid.full()];
		QString name;

		QString cs_str = cs.flatten();
		if (CapsRegistry::instance()->isRegistered(cs_str)) {
			DiscoItem disco = CapsRegistry::instance()->disco(cs_str);
			XData si = disco.registeredExtension(QLatin1String("urn:xmpp:dataforms:softwareinfo"));
			if (si.isValid()) {
				name = si.getField("software").value().value(0);
			}

			if (name.isEmpty()) {
				const DiscoItem::Identities& i = disco.identities();
				if (i.count() > 0) {
					name = i.first().name;
				}
			}
		}

		// Try to be intelligent about the name
		if (name.isEmpty()) {
			name = cs.node();
			if (name.startsWith("http://"))
				name = name.right(name.length() - 7);
			else if (name.startsWith("https://"))
				name = name.right(name.length() - 8);

			if (name.startsWith("www."))
				name = name.right(name.length() - 4);

			int cut_pos = name.indexOf("/");
			if (cut_pos != -1)
				name = name.left(cut_pos);
		}

		return name;
	}
	else {
		return QString();
	}
}

/**
 * \brief Returns the client version of a given jid.
 */
QString CapsManager::clientVersion(const Jid& jid) const
{
	if (!capsEnabled(jid))
		return QString();

	QString version;
	const CapsSpec &cs = capsSpecs_[jid.full()];
	QString cs_str = cs.flatten();
	if (CapsRegistry::instance()->isRegistered(cs_str)) {
		XData form = CapsRegistry::instance()->disco(cs_str).registeredExtension("urn:xmpp:dataforms:softwareinfo");
		version = form.getField("software_version").value().value(0);
	}

	return version;
}

/**
 * \brief Returns the OS version of a given jid.
 */
QString CapsManager::osVersion(const Jid &jid) const
{
	QString os_str;
	if (capsEnabled(jid)) {
		QString cs_str = capsSpecs_[jid.full()].flatten();
		if (CapsRegistry::instance()->isRegistered(cs_str)) {
			XData form = CapsRegistry::instance()->disco(cs_str).registeredExtension("urn:xmpp:dataforms:softwareinfo");
			os_str = form.getField("os").value().value(0).trimmed();
			if (!os_str.isEmpty()) {
				QString os_ver = form.getField("os_version").value().value(0).trimmed();
				if (!os_ver.isEmpty())
					os_str.append(" " + os_ver);
			}
		}
	}
	return os_str;
}

CapsSpec CapsManager::capsSpec(const Jid &jid) const
{
	return capsSpecs_.value(jid.full());
}

} // namespace XMPP
