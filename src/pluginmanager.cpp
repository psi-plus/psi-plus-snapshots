/*
 * (c) 2006 Kevin Smith
 * (c) 2008 Maciej Niedzielski
 */

#include "pluginmanager.h"

#include <QtCore>
#include <QtCrypto>
#include <QPluginLoader>
#include <QLabel>

#include "xmpp_client.h"
#include "xmpp_task.h"
#include "xmpp_message.h"

#include "applicationinfo.h"
#include "psioptions.h"

#include "pluginhost.h"
#include "psicon.h"
#include "psicontact.h"
#include "psiplugin.h"
#include "psiaccount.h"
#include "stanzafilter.h"
#include "stanzasender.h"
#include "iqfilter.h"
#include "iqnamespacefilter.h"
#include "eventfilter.h"
#include "optionaccessor.h"
#include "avatars.h"
#include "psiiconset.h"
#include "popupmanager.h"


//TODO(mck)
// - use native separators when displaying file path


/**
 * Helper class used to process incoming XML in plugins.
 */
class PluginManager::StreamWatcher: public XMPP::Task
{
public:
	StreamWatcher(Task* t, PluginManager* m, int a) : Task(t), manager(m), account(a) {}
	bool take(const QDomElement& e) {
		return manager->incomingXml(account, e);
	}
	PluginManager* manager;
	int account;
};


/**
 * Function to obtain all the directories in which plugins can be stored
 * \return List of plugin directories
 */ 
static QStringList pluginDirs()
{
	return ApplicationInfo::pluginDirs();
}

/**
 * Method for accessing the singleton instance of the class.
 * Instanciates if no instance yet exists.
 * \return Pointer to the plugin manager.
 */
PluginManager* PluginManager::instance()
{
	if (!instance_) {
		instance_ = new PluginManager();
	}
	return instance_;
}

/**
 * Default constructor. Locates all plugins, sets watchers on those directories to 
 * locate new ones and loads those enabled in the config.
 */ 
PluginManager::PluginManager() : QObject(NULL), psi_(0)
{
	updatePluginsList();
	foreach (QString path, pluginDirs()) {
		QCA::DirWatch *dw = new QCA::DirWatch(path, this);
		connect(dw, SIGNAL(changed()), SLOT(dirsChanged()));
		dirWatchers_.append(dw);
	}
	connect(PsiOptions::instance(), SIGNAL(optionChanged(const QString&)), this, SLOT(optionChanged(const QString&)));
}

void PluginManager::initNewSession(PsiCon *psi)
{
	psi_ = psi;
	clients_.clear();
	accountIds_.clear();
	loadEnabledPlugins();
}

/**
 * Updates list of known plugins by reading all plugin directories
 * and returns the list of new plugins
 */
QList<PluginHost*> PluginManager::updatePluginsList()
{
	QList<PluginHost*> newPlugins;

	foreach (QString d, pluginDirs()) {
		QDir dir(d);
		foreach (QString file, dir.entryList(QDir::Files)) {
			file = dir.absoluteFilePath(file);
			if (QLibrary::isLibrary(file)) {
				qWarning("Found plugin: %s", qPrintable(file));
				if (!pluginByFile_.contains(file)) {
					PluginHost* host = new PluginHost(this, file);
					if (host->isValid() && !hosts_.contains(host->name())) {
						hosts_[host->name()] = host;
						pluginByFile_[file] = host;
						newPlugins.append(host);
						if (host->priority() == PsiPlugin::PriorityHighest || !pluginsByPriority_.size()) {
							pluginsByPriority_.push_front(host);
						} else {
							// find position for insertion
							int i = pluginsByPriority_.size();
							while (i && host->priority() > pluginsByPriority_[i - 1]->priority()) {
								i--;
							}
							pluginsByPriority_.insert(i, host);
						}
					}
				} else {
					qWarning("Which we already knew about");
				}
			}
		}
	}

	return newPlugins;
}

/**
 * This slot is executed when the contents of a plugin directory changes
 * It causes the available plugin list to be refreshed.
 */ 
void PluginManager::dirsChanged()
{
	foreach(PluginHost* plugin, updatePluginsList()) {
		loadPluginIfEnabled(plugin);
	}
}

void PluginManager::accountDestroyed()
{
	PsiAccount* pa = static_cast<PsiAccount*>(sender());
	const int index = accountIds_.value(pa);
	if(index != -1) {
		accountIds_.insert(0, index);
	}
}

/**
 * This causes all plugins that are both set for auto-load, and available
 * to be loaded. 
 */
void PluginManager::loadEnabledPlugins()
{
	qDebug("Loading enabled plugins");
	foreach (PluginHost* plugin, pluginsByPriority_) {
		loadPluginIfEnabled(plugin);
	}
}

void PluginManager::loadPluginIfEnabled(PluginHost *plugin)
{
	const QString option = QString("%1.%2").arg(loadOptionPrefix).arg(plugin->shortName());
	if (PsiOptions::instance()->getOption(option).toBool()) {
		qWarning("Plugin %s is enabled in config: loading", qPrintable(plugin->shortName()));
		plugin->enable();
	}
}

void PluginManager::loadUnloadPlugin(QString pluginName, bool load)
{
	if (hosts_.contains(pluginName)) {
		PluginHost* plugin = hosts_[pluginName];
		QString option = QString("%1.%2").arg(loadOptionPrefix).arg(plugin->shortName());
		PsiOptions::instance()->setOption(option, QVariant(load));
 		if(load) {            
			plugin->enable();
		} else {
			if(optionsWidget_)
				delete optionsWidget_;
			plugin->unload();
		}
	}
}

/**
 * Called when an option changes to load or unload a plugin if it's a plugin 
 * option
 * \param option Option changed
 */
void PluginManager::optionChanged(const QString& option)
{
	//QString("%1.%2").arg(loadOptionPrefix).arg(shortNames_[plugin]);

	//TODO(mck): implement this... for now, enabling/disabling requires psi restart

	foreach (PluginHost* plugin, pluginByFile_.values()) {
		plugin->optionChanged(option);
	}
}

/**
 * Loads all available plugins
 */ 
void PluginManager::loadAllPlugins()
{
	qDebug("Loading all plugins");
  	//Any static (compiled in) plugins we happen to have
	/*foreach( QObject* plugin, QPluginLoader::staticInstances() ) {
		loadPlugin( plugin );
	}*/

	//Now look for external plugins
	foreach (PluginHost* plugin, hosts_.values()) {
		plugin->load();
		plugin->enable();
	}
}

/**
 * Unloads all Psi plugins. 
 * \return Success of unloading all plugins; if any plugins couldn't be 
 *         unloaded, false.
 */ 
bool PluginManager::unloadAllPlugins()
{
	qDebug("Unloading all plugins");
	bool ok = true;
	foreach (PluginHost* plugin, hosts_.values()) {
		if (!plugin->unload()) {
			ok = false;
		}
	}
	return ok;
}

/**
 * Find the file which provides the named plugin. If the named plugin is not
 * known, an empty string is provided.
 * \param plugin Name of the plugin.
 * \return Path to the plugin file.
 */ 
QString PluginManager::pathToPlugin(const QString& plugin) const
{
	QString path;
	if (hosts_.contains(plugin)) {
		path = hosts_[plugin]->path();
	}
	return path;
}

/**
 * Returns short name of the named plugin. If the named plugin is not
 * known, an empty string is provided.
 * \param plugin Name of the plugin.
 * \return Path to the plugin file.
 */ 
QString PluginManager::shortName(const QString& plugin) const
{
	QString name;
	if (hosts_.contains(plugin)) {
		name = hosts_[plugin]->shortName();
	}
	return name;
}

QString PluginManager::version(const QString& plugin) const
{
	QString name;
	if (hosts_.contains(plugin)) {
		name = hosts_[plugin]->version();
	}
	return name;
}

/**
 * Returns a list of available plugin names found in all plugin directories.
 */
QStringList PluginManager::availablePlugins() const
{
	return hosts_.keys();
}

/**
 * Provides a pointer to a QWidget providing the options dialog for the 
 * named plugin, if it exists, else NULL.
 * \param plugin Name of the plugin.
 * \return Pointer to the options widget for the named plugin.
 */
QWidget* PluginManager::optionsWidget(const QString& plugin)
{
	if(optionsWidget_) {
		delete optionsWidget_;
	}

	if (hosts_.contains(plugin)) {
		optionsWidget_ = hosts_[plugin]->optionsWidget();
	}

	if(!optionsWidget_) {
		qWarning("Attempting to get options for %s which doesn't exist", qPrintable(plugin));
		optionsWidget_ = new QLabel(tr("This plugin has no user configurable options"));
	}
	return optionsWidget_;
}

/**
 * \brief Give each plugin the opportunity to set shortcuts
 *
 * Each plugin can set its own global shortcuts
 */
void PluginManager::setShortcuts()
{
	foreach (PluginHost* host, pluginByFile_.values()) {
		host->setShortcuts();
	}
}

/**
 * \brief Give each plugin the opportunity to process the incoming message event
 *
 * Each plugin is passed the event in turn. Any plugin may then modify the event
 * and may cause the event to be silently discarded.
 *
 * \param account Pointer to the PsiAccount responsible
 * \param event Incoming event
 * \return Continue processing the event; true if the event should be silently discarded.
 */
bool PluginManager::processMessage(PsiAccount* account, const QString& jidFrom, const QString& body, const QString& subject)
{
	bool handled = false;
	foreach (PluginHost* host, pluginsByPriority_) {
		if (host->processMessage(accountIds_[account], jidFrom, body, subject)) {
			handled = true;
			break;
		}
	}
	return handled;
}

/**
 * \brief Give each plugin the opportunity to process the incoming event
 * 
 * Each plugin is passed the event in turn. Any plugin may then modify the event
 * and may cause the event to be silently discarded.
 * 
 * \param account Pointer to the PsiAccount responsible
 * \param event Incoming event
 * \return Continue processing the event; true if the event should be silently discarded.
 */
bool PluginManager::processEvent(PsiAccount* account, QDomElement& event)
{
	bool handled = false;
	foreach (PluginHost* host, pluginsByPriority_) {
		if (host->processEvent(accountIds_[account], event)) {
			handled = true;
			break;
		}
	}
	return handled;
}

/**
 * process an outgoing message
 */
bool PluginManager::processOutgoingMessage(PsiAccount* account, const QString& jidTo, QString& body, const QString& type, QString& subject)
{
	bool handled = false;
	foreach (PluginHost* host, pluginByFile_.values()) {
		if (host->processOutgoingMessage(accountIds_[account], jidTo, body, type, subject)) {
			handled = true;
			break;
		}
	}
	return handled;
}

void PluginManager::processOutgoingStanza(PsiAccount* account, QDomElement &stanza)
{
	foreach (PluginHost* host, pluginByFile_.values()) {
		if (host->outgoingXml(accountIds_[account], stanza)) {
			break;
		}
	}
}

/**
 * Notify to plugins that an account will go offline now.
 */
void PluginManager::logout(PsiAccount* account)
{
	foreach (PluginHost* host, pluginByFile_.values()) {
		host->logout(accountIds_[account]);
	}
}

/**
 * \brief Give each plugin the opportunity to process the incoming xml
 *
 * Each plugin is passed the xml in turn using various filter interfaces
 * (for example, see StanzaFilter or IqFilter).
 * Any plugin may then modify the xml and may cause the stanza to be
 * silently discarded.
 * 
 * \param account Identifier of the PsiAccount responsible
 * \param xml Incoming XML
 * \return Continue processing the event; true if the event should be silently discarded.
 */
bool PluginManager::incomingXml(int account, const QDomElement &xml)
{
	bool handled = false;
	foreach (PluginHost* host, pluginsByPriority_) {
		if (host->incomingXml(account, xml)) {
			handled = true;
			break;
		}
	}
	return handled;
}

/**
 * Called by PluginHost when its hosted plugin wants to send xml stanza.
 *
 * \param account Identifier of the PsiAccount responsible
 * \param xml XML stanza to be sent
 */
void PluginManager::sendXml(int account, const QString& xml)
{
	//TODO(mck)
	// - think if it is better to ask plugin(host) for string or xml node
	// - validate things
	// - add id if missing
	// - maybe use appropriate Task to send
	// - make psi aware of things that are being send
	//   (for example, pipeline messages through history system)

	if (account < clients_.size()) {
		clients_[account]->send(xml);
	}
}

/**
 * Returns unique stanza identifier in account's stream
 *
 * \param account Identifier of the PsiAccount responsible
 * \return Unique ID to be used for when sending a stanza
 */
QString PluginManager::uniqueId(int account) const
{
	QString id;
	if (account < clients_.size()) {
		id = clients_[account]->genUniqueId();
	}
	return id;
}

QString PluginManager::getStatus(int account) const
{
	Status S;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa)
			S = pa->status();
	}
	return S.typeString();
}

QString PluginManager::getStatusMessage(int account) const
{
	Status S;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa)
			S = pa->status();
	}
	return S.status();
}

static inline const QString getProxyId(PsiAccount* pa)
{
	return pa->userAccount().proxyID;
}

QString PluginManager::proxyHost(int account) const
{
	QString host;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			host = ProxyManager::instance()->getItem(getProxyId(pa)).settings.host;
		}
	}
	return host;
}

int PluginManager::proxyPort(int account) const
{
	int port = -1;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			port = ProxyManager::instance()->getItem(getProxyId(pa)).settings.port;
		}
	}
	return port;
}

QString PluginManager::proxyUser(int account) const
{
	QString user;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			user = ProxyManager::instance()->getItem(getProxyId(pa)).settings.user;
		}
	}
	return user;
}

QString PluginManager::proxyPassword(int account) const
{
	QString pass;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			pass = ProxyManager::instance()->getItem(getProxyId(pa)).settings.pass;
		}
	}
	return pass;
}

QStringList PluginManager::getRoster(int account) const
{
	QStringList list;
	list << "-1";
	if (account < accountIds_.size()) {
		list.clear();
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			QList<PsiContact*> roster = pa->contactList();
			for(int i = 0; i < roster.size(); i++) {
				list.push_back(roster.at(i)->jid().bare());
			}
		}
	}
	return list;
}

QString PluginManager::getJid(int account) const
{
	QString Jid = "-1";
	if (account < accountIds_.size()) {
		Jid.clear();
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			Jid = pa->jid().bare();
		}
	}
	return Jid;
}

QString PluginManager::getId(int account) const
{
	QString id = "-1";
	if (account < accountIds_.size()) {
		id.clear();
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			id = pa->id();
		}
	}
	return id;
}

QString PluginManager::getName(int account) const
{
	QString nm;
	if (account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			nm = pa->name();
		}
	}
	return nm;
}

bool PluginManager::setActivity(int account, const QString& jid, QDomElement xml)
{
	if (account >= accountIds_.size())
		return false;

	PsiAccount *pa = accountIds_.key(account);
	if(!pa) {
		return false;
	}
	XMPP::Jid userJid(jid);
	UserListItem *item =  pa->userList()->find(userJid);

	if(!item)
		return false;

	Activity act = Activity();
	if(!xml.isNull())
		act = Activity(xml);
	item->setActivity(act);
	return true;
}

bool PluginManager::setMood(int account, const QString& jid, QDomElement xml)
{
	if (account >= accountIds_.size())
		return false;

	PsiAccount *pa = accountIds_.key(account);
	if(!pa) {
		return false;
	}
	XMPP::Jid userJid(jid);
	UserListItem *item =  pa->userList()->find(userJid);

	if(!item)
		return false;

	Mood mood = Mood();
	if(!xml.isNull())
		mood = Mood(xml);
	item->setMood(mood);
	return true;
}

bool PluginManager::setTune(int account, const QString& jid, const QString& tune)
{
	if (account >= accountIds_.size())
		return false;

	PsiAccount *pa = accountIds_.key(account);
	if(!pa) {
		return false;
	}
	XMPP::Jid userJid(jid);
	UserListItem *item =  pa->userList()->find(userJid);

	if(!item)
		return false;

	item->setTune(tune);
	return true;
}

void PluginManager::initPopup(const QString& text, const QString& title, const QString& icon)
{
	const PsiIcon* ico = IconsetFactory::iconPtr(icon);
	PopupManager::doPopup(0, Jid(), ico, title, 0, 0, text);
}

void PluginManager::initPopupForJid(int account, const QString &jid, const QString &text, const QString &title, const QString &icon)
{
	XMPP::Jid j(jid);
	const PsiIcon* ico = IconsetFactory::iconPtr(icon);
	if (account < accountIds_.size()) {
		PsiAccount * pa = accountIds_.key(account);
		if(pa) {
			UserListItem *i = pa->findFirstRelevant(j);
			PsiIcon *statusIco = PsiIconset::instance()->statusPtr(i);
			const QPixmap pix = pa->avatarFactory()->getAvatar(j);
			PopupManager::doPopup(pa, j, ico, title, &pix, statusIco, text);
			return;
		}
	}
	PopupManager::doPopup(0, Jid(), ico, title, 0, 0, text);
}

void PluginManager::registerOption(const QString& name, int initValue, const QString& path)
{
	psi_->popupManager()->registerOption(name, initValue, path);
}

int PluginManager::popupDuration(const QString& name) const
{
	return psi_->popupManager()->value(name);
}

void PluginManager::setPopupDuration(const QString& name, int value)
{
	psi_->popupManager()->setValue(name, value);
}

void PluginManager::addAccountMenu(QMenu *menu, PsiAccount* account)
{
	int i = accountIds_.value(account);
	foreach (PluginHost* host, pluginsByPriority_) {
		host->addAccountMenu(menu, i);
	}
}

void PluginManager::addContactMenu(QMenu* menu, PsiAccount *account, QString jid)
{
	int i = accountIds_.value(account);
	foreach (PluginHost* host, pluginsByPriority_) {
		host->addContactMenu(menu, i, jid);
	}
}

bool PluginManager::hasInfoProvider(const QString& plugin) const
{
	if (hosts_.contains(plugin))
		return hosts_[plugin]->hasInfoProvider();

	return false;
}

QString PluginManager::pluginInfo(const QString& plugin) const
{
	QString info;
	if (hosts_.contains(plugin))
		info = hosts_[plugin]->pluginInfo();
	return info;
}

/**
 * Tells the plugin manager about an XMPP::Client and the owning PsiAccount
 */
void PluginManager::addAccount(PsiAccount* account, XMPP::Client* client)
{
	clients_.append(client);
	const int id = clients_.size() - 1;
	accountIds_[account] = id;
	new StreamWatcher(client->rootTask(), this, id);
	connect(account, SIGNAL(accountDestroyed()), this, SLOT(accountDestroyed()));
}

/**
 * Performs basic validity checking on a stanza
 * TODO : populate verifyStanza method and use it
 */
bool PluginManager::verifyStanza(const QString& stanza)
{
	Q_UNUSED(stanza);
	return true;
}

void PluginManager::applyOptions(const QString& plugin)
{
	if (hosts_.contains(plugin)) {
		hosts_[plugin]->applyOptions();
	}
}

void PluginManager::restoreOptions(const QString& plugin)
{
	if (hosts_.contains(plugin)) {
		hosts_[plugin]->restoreOptions();
	}
}

void PluginManager::addToolBarButton(QObject* parent, QWidget* toolbar, PsiAccount* account, const QString& contact)
{

	foreach (PluginHost* host, pluginsByPriority_) {
		host->addToolBarButton(parent,toolbar, accountIds_[account], contact);
	}
}

void PluginManager::addGCToolBarButton(QObject* parent, QWidget* toolbar, PsiAccount* account, const QString& contact)
{

	foreach (PluginHost* host, pluginsByPriority_) {
		host->addGCToolBarButton(parent,toolbar, accountIds_[account], contact);
	}
}

void PluginManager::setStatus(int account, const QString& status, const QString& statusMessage)
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			XMPP::Status s(status, statusMessage);
			acc->setStatus(s, false, true);
		}
	}
}

void PluginManager::createNewEvent(int account, const QString &jid, const QString &descr, QObject *receiver, const char *slot)
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			acc->createNewPluginEvent(jid, descr, receiver, slot);
		}
	}
}

bool PluginManager::isSelf(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->isSelf();
		}
	}
	return false;
}

bool PluginManager::isAgent(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->isAgent();
		}
	}
	return false;
}

bool PluginManager::inList(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->inList();
		}
	}
	return false;
}

bool PluginManager::isPrivate(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->isPrivate();
		}
	}
	return false;
}

bool PluginManager::isConference(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->isConference();
		}
	}
	return false;
}

QString PluginManager::name(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->name();
		}
	}
	return QString();
}

QString PluginManager::status(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->status().typeString();
		}
	}
	return QString();
}

QString PluginManager::statusMessage(int account, const QString& jid) const
{
	if(account < accountIds_.size()) {
		PsiAccount *acc = accountIds_.key(account);
		if(acc) {
			PsiContact *pc = acc->findContact(XMPP::Jid(jid));
			if(pc)
				return pc->status().status();
	}
	}
	return QString();
}

QStringList PluginManager::resources(int account, const QString& jid) const
{
	QStringList l;
	if(account < accountIds_.size()) {
		PsiAccount *pa = accountIds_.key(account);
		if(pa) {
			UserListItem *u = pa->findFirstRelevant(XMPP::Jid(jid));
			if(u) {
				QMutableListIterator<UserResource> i(u->userResourceList());
				while (i.hasNext()) {
					l.push_back(i.next().name());
				}
			}
		}
	}
	return l;
}

PluginManager* PluginManager::instance_ = NULL;
const QString PluginManager::loadOptionPrefix = "plugins.auto-load";
const QString PluginManager::pluginOptionPrefix = "plugins.options";
