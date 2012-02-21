#include <QString>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QLocale>

#ifdef Q_WS_X11
#include <sys/stat.h> // chmod
#endif

#ifdef Q_WS_WIN
#include <windows.h>
#endif

#ifdef Q_WS_MAC
#include <sys/stat.h> // chmod
#include <CoreServices/CoreServices.h>
#endif

#include "psiapplication.h"
#include "applicationinfo.h"
#include "systeminfo.h"
#include "profiles.h"
#include "homedirmigration.h"
#include "activeprofiles.h"
#include "translationmanager.h"
#ifdef HAVE_CONFIG
#include "config.h"
#endif

// Constants. These should be moved to a more 'dynamically changeable'
// place (like an external file loaded through the resources system)
// Should also be overridable through an optional file.

#define PROG_NAME "Psi+"
#ifdef WEBKIT
#define PROG_VERSION "0.15.5217-webkit" " (" __DATE__ ")" //CVS Builds are dated
#else
#define PROG_VERSION "0.15.5217" " (" __DATE__ ")" //CVS Builds are dated
#endif
//#define PROG_VERSION "0.15";
#define PROG_CAPS_NODE "http://psi-dev.googlecode.com/caps"
#define PROG_CAPS_VERSION "0.15"
#define PROG_IPC_NAME "org.psi-im.Psi"	// must not contain '\\' character on Windows
#define PROG_OPTIONS_NS "http://psi-im.org/options"
#define PROG_STORAGE_NS "http://psi-im.org/storage"
#define PROG_FILECACHE_NS "http://psi-im.org/filecache"
#ifdef Q_WS_MAC
#define PROG_APPCAST_URL "http://psi-im.org/appcast/psi-mac.xml"
#else
#define PROG_APPCAST_URL ""
#endif

#if defined(Q_WS_X11) && !defined(PSI_DATADIR)
#define PSI_DATADIR "/usr/local/share/psi"
#endif


QString ApplicationInfo::name()
{
	return PROG_NAME;
}


QString ApplicationInfo::version()
{
	return PROG_VERSION;
}

QString ApplicationInfo::capsNode()
{
	return PROG_CAPS_NODE;
}

QString ApplicationInfo::capsVersion()
{
	return PROG_CAPS_VERSION;
}

QString ApplicationInfo::osName()
{
	return SystemInfo::instance()->os();
}

QString ApplicationInfo::IPCName()
{
	return PROG_IPC_NAME;
}

QString ApplicationInfo::getAppCastURL()
{
	return PROG_APPCAST_URL;
}

QString ApplicationInfo::optionsNS()
{
	return PROG_OPTIONS_NS;
}

QString ApplicationInfo::storageNS()
{
	return PROG_STORAGE_NS;
}	

QString ApplicationInfo::fileCacheNS()
{
	return PROG_FILECACHE_NS;
}

QStringList ApplicationInfo::getCertificateStoreDirs()
{
	QStringList l;
	l += ApplicationInfo::resourcesDir() + "/certs";
	l += ApplicationInfo::homeDir(ApplicationInfo::DataLocation) + "/certs";
	return l;
}

QStringList ApplicationInfo::dataDirs()
{
	const static QStringList dirs = QStringList() << ":" << "." << homeDir(DataLocation)
												  << resourcesDir();
	return  dirs;
}

QStringList ApplicationInfo::pluginDirs()
{
	QStringList l;
	l += resourcesDir() + "/plugins";
	l += homeDir(ApplicationInfo::DataLocation) + "/plugins";
#if defined(Q_OS_UNIX)
	l += libDir() + "/plugins";
#endif
	return l;
}

QString ApplicationInfo::getCertificateStoreSaveDir()
{
	QDir certsave(homeDir(DataLocation) + "/certs");
	if(!certsave.exists()) {
		QDir home(homeDir(DataLocation));
		home.mkdir("certs");
	}

	return certsave.path();
}

QString ApplicationInfo::resourcesDir()
{
#if defined(Q_WS_X11)
	return PSI_DATADIR;
#elif defined(Q_WS_WIN)
	return qApp->applicationDirPath();
#elif defined(Q_WS_MAC)
	// FIXME: Clean this up (remko)
	// System routine locates resource files. We "know" that Psi.icns is
	// in the Resources directory.
	QString resourcePath;
	CFBundleRef mainBundle = CFBundleGetMainBundle();
	CFStringRef resourceCFStringRef = CFStringCreateWithCString(NULL,
									"application.icns", kCFStringEncodingASCII);
	CFURLRef resourceURLRef = CFBundleCopyResourceURL(mainBundle,
											resourceCFStringRef, NULL, NULL);
	if (resourceURLRef) {
		CFStringRef resourcePathStringRef = CFURLCopyFileSystemPath(
					resourceURLRef, kCFURLPOSIXPathStyle);
		const char* resourcePathCString = CFStringGetCStringPtr(
					resourcePathStringRef, kCFStringEncodingASCII);
		if (resourcePathCString) {
			resourcePath = resourcePathCString;
		}
		else { // CFStringGetCStringPtr failed; use fallback conversion
			CFIndex bufferLength = CFStringGetLength(resourcePathStringRef) + 1;
			char* resourcePathCString = new char[ bufferLength ];
			Boolean conversionSuccess = CFStringGetCString(
						resourcePathStringRef, resourcePathCString,
						bufferLength, kCFStringEncodingASCII);
			if (conversionSuccess) {
				resourcePath = resourcePathCString;
			}
			delete [] resourcePathCString;  // I own this
		}
		CFRelease( resourcePathStringRef ); // I own this
	}
	// Remove the tail component of the path
	if (! resourcePath.isNull()) {
		QFileInfo fileInfo(resourcePath);
		resourcePath = fileInfo.absolutePath();
	}
	return resourcePath;
#endif
}

QString ApplicationInfo::libDir()
{
#if defined(Q_OS_UNIX)
	return PSI_LIBDIR;
#else
	return QString();
#endif
}

/** \brief return psi's private read write data directory
  * unix+mac: $HOME/.psi
  * environment variable "PSIDATADIR" overrides
  */
QString ApplicationInfo::homeDir(ApplicationInfo::HomedirType type)
{
	static QString configDir_;
	static QString dataDir_;
	static QString cacheDir_;

	if (configDir_.isEmpty()) {
		// Try the environment override first
		configDir_ = QString::fromLocal8Bit(getenv("PSIPLUSDATADIR"));

		if (configDir_.isEmpty()) {
#if defined Q_WS_WIN
			QString base;
			if (((PsiApplication *)qApp)->portableBase().isEmpty()) {
				base = QString::fromLocal8Bit(getenv("appdata"));
			}
			else {
				base = ((PsiApplication *)qApp)->portableBase();
			}
			QDir configDir(base + "/" + name());
			QDir cacheDir(configDir);
			QDir dataDir(configDir);
#elif defined Q_WS_MAC
			QDir configDir(QDir::homePath() + "/Library/Application Support/" + name());
			QDir cacheDir(QDir::homePath() + "/Library/Caches/" + name());
			QDir dataDir(configDir);
#elif defined Q_WS_X11
			QString XdgConfigHome = QString::fromLocal8Bit(getenv("XDG_CONFIG_HOME"));
			QString XdgDataHome = QString::fromLocal8Bit(getenv("XDG_DATA_HOME"));
			QString XdgCacheHome = QString::fromLocal8Bit(getenv("XDG_CACHE_HOME"));
			if (XdgConfigHome.isEmpty()) {
				XdgConfigHome = QDir::homePath() + "/.config";
			}
			if (XdgDataHome.isEmpty()) {
				XdgDataHome = QDir::homePath() + "/.local/share";
			}
			if (XdgCacheHome.isEmpty()) {
				XdgCacheHome = QDir::homePath() + "/.cache";
			}
			QDir configDir(XdgConfigHome + "/" + name());
			QDir dataDir(XdgDataHome + "/" + name());
			QDir cacheDir(XdgCacheHome + "/" + name());
#endif
			configDir_ = configDir.path();
			cacheDir_ = cacheDir.path();
			dataDir_ = dataDir.path();

			// To prevent from multiple startup of import  wizard
			if (ActiveProfiles::instance()->isActive("import_wizard")) {
				exit(0);
			}

			if (!configDir.exists() && !dataDir.exists() && !cacheDir.exists()) {
				HomeDirMigration dlg;

				if (dlg.checkOldHomeDir()) {
					ActiveProfiles::instance()->setThisProfile("import_wizard");
					QSettings s(dlg.oldHomeDir() + "/psirc", QSettings::IniFormat);
					QString lastLang = s.value("last_lang", QString()).toString();
					if(lastLang.isEmpty()) {
						lastLang = QLocale().name().section('_', 0, 0);
					}
					TranslationManager::instance()->loadTranslation(lastLang);
					dlg.exec();
					ActiveProfiles::instance()->unsetThisProfile();
				}
			}
			if (!dataDir.exists()) {
				dataDir.mkpath(".");
			}
			if (!cacheDir.exists()) {
				cacheDir.mkpath(".");
			}
		}
		else {
			cacheDir_ = configDir_;
			dataDir_ = configDir_;
		}
	}

	QString ret;
	switch(type) {
	case ApplicationInfo::ConfigLocation:
		ret = configDir_;
		break;

	case ApplicationInfo::DataLocation:
		ret = dataDir_;
		break;

	case ApplicationInfo::CacheLocation:
		ret = cacheDir_;
		break;
	}
	return ret;
}

QString ApplicationInfo::makeSubhomePath(const QString &path, ApplicationInfo::HomedirType type)
{
	if (path.indexOf("..") == -1) { // ensure its in home dir
		QDir dir(homeDir(type) + "/" + path);
		if (!dir.exists()) {
			dir.mkpath(".");
		}
		return dir.path();
	}
	return QString();
}

QString ApplicationInfo::makeSubprofilePath(const QString &path, ApplicationInfo::HomedirType type)
{
	if (path.indexOf("..") == -1) { // ensure its in profile dir
		QDir dir(pathToProfile(activeProfile, type) + "/" + path);
		if (!dir.exists()) {
			dir.mkpath(".");
		}
		return dir.path();
	}
	return QString();
}

QString ApplicationInfo::historyDir()
{
	return makeSubprofilePath("history", ApplicationInfo::DataLocation);
}

QString ApplicationInfo::vCardDir()
{
	return makeSubprofilePath("vcard", ApplicationInfo::CacheLocation);
}

QString ApplicationInfo::bobDir()
{
	return makeSubhomePath("bob", ApplicationInfo::CacheLocation);
}

QString ApplicationInfo::currentProfileDir(ApplicationInfo::HomedirType type)
{
	return pathToProfile(activeProfile, type);
}

QString ApplicationInfo::profilesDir(ApplicationInfo::HomedirType type)
{
	return makeSubhomePath("profiles", type);
}

QString ApplicationInfo::desktopFile()
{
	QString dFile;
	const QString execFileName = QFileInfo(qApp->applicationFilePath()).fileName();
	const QString desktopFile = QString("/usr/share/applications/%1.desktop").arg(execFileName);
	QFile f(desktopFile);
	if(f.open(QIODevice::ReadOnly)) {
		dFile = QString::fromUtf8(f.readAll());
	}
	else {
		dFile = "[Desktop Entry]\n";
		dFile += "Version=1.0\n";
		dFile += "Type=Application\n";
		dFile += QString("Name=%1\n").arg(name());
		dFile += "GenericName=Jabber Client\n";
		dFile += "Comment=Communicate over the Jabber network\n";
		dFile += QString("Icon=%1\n").arg(execFileName);
		dFile += QString("Exec=%1\n").arg(execFileName);
		dFile += "Terminal=false\n";
		dFile += "Categories=Network;InstantMessaging;Qt;";
	}

	return dFile;
}
