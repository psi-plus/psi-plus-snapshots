/*
 * chatviewtheme.cpp - theme for webkit based chatview
 * Copyright (C) 2010 Rion (Sergey Ilinyh)
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

#ifdef QT_WEBENGINEWIDGETS_LIB
#include <QWebEnginePage>
#include <QWebChannel>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineProfile>
#include <functional>
#else
#include <QWebPage>
#include <QWebFrame>
#include <QNetworkRequest>
#endif
#include <QFileInfo>
#include <QApplication>
#include <QScopedPointer>
#include <time.h>
#include <tuple>

#include "chatviewtheme.h"
#include "psioptions.h"
#include "coloropt.h"
#include "jsutil.h"
#include "webview.h"
#include "chatviewthemeprovider.h"
#ifdef QT_WEBENGINEWIDGETS_LIB
# include "themeserver.h"
#endif
#include "avatars.h"
#include "common.h"

#ifndef QT_WEBENGINEWIDGETS_LIB
# ifdef HAVE_QT5
#  define QT_JS_QTOWNERSHIP QWebFrame::QtOwnership
# else
#  define QT_JS_QTOWNERSHIP QScriptEngine::QtOwnership
# endif
#endif


class ChatViewThemePrivate : public QSharedData
{
public:
	QString html;
	QString httpRelPath;
	QScopedPointer<ChatViewJSLoader> jsLoader;
	QScopedPointer<ChatViewThemeJSUtil> jsUtil;// it's abslutely the same object for every theme.
	QPointer<WebView> wv;
	QMap<QString,QVariant> cache;
	bool prepareSessionHtml = false; // if html should be generated by JS for each session.
	bool transparentBackground = false;

#ifdef QT_WEBENGINEWIDGETS_LIB
	QList<QWebEngineScript> scripts;
#else
	QStringList scripts;
#endif
	std::function<void(bool)> loadCallback;

#ifndef QT_WEBENGINEWIDGETS_LIB
	QVariant evaluateFromFile(const QString fileName, QWebFrame *frame)
	{
		QFile f(fileName);
		if (f.open(QIODevice::ReadOnly)) {
			return frame->evaluateJavaScript(f.readAll());
		}
		return QVariant();
	}
#endif
};

class ChatViewJSLoader : public QObject
{
	Q_OBJECT

	ChatViewTheme *theme;
	QString _loadError;
	QHash<QString, QObject*> _sessions;

	Q_PROPERTY(QString themeId READ themeId CONSTANT)
	Q_PROPERTY(QString isMuc READ isMuc CONSTANT)
	Q_PROPERTY(QString serverUrl READ serverUrl CONSTANT)

signals:
	void sessionHtmlReady(const QString &sessionId, const QString &html);

public:
	ChatViewJSLoader(ChatViewTheme *theme, QObject *parent = 0) :
	    QObject(parent),
	    theme(theme)
	{}

	const QString themeId() const
	{
		return theme->id();
	}

	bool isMuc() const
	{
		return theme->isMuc();
	}

	QString serverUrl() const
	{
#if QT_WEBENGINEWIDGETS_LIB
		ChatViewThemeProvider *provider = static_cast<ChatViewThemeProvider*>(theme->themeProvider());
		auto server = provider->themeServer();
		QUrl url = server->serverUrl();
		return url.url();
#else
		static QString url("http://psi");
		return url;
#endif
	}

	void registerSession(const QSharedPointer<ChatViewThemeSession> &session)
	{
		_sessions.insert(session->sessionId(), session->jsBridge());
	}

	void unregisterSession(const QString &sessId)
	{
		_sessions.remove(sessId);
	}

public slots:
	void setMetaData(const QVariantMap &map)
	{
		if (map["name"].isValid()) {
			theme->setName(map["name"].toString());
		}
	}

	void finishThemeLoading()
	{
		qDebug("%s theme is successfully loaded", qPrintable(theme->id()));
		theme->cvtd->loadCallback(true);
	}

	void errorThemeLoading(const QString &error)
	{
		_loadError = error;
		theme->cvtd->loadCallback(false);
	}

	void setHtml(const QString &h)
	{
		theme->cvtd->html = h;
	}

	void setHttpResourcePath(const QString &relPath)
	{
		theme->cvtd->httpRelPath = relPath;
	}


	// we don't need not text cache since binary data(images?)
	// most likely will be cached by webkit itself
	void toCache(const QString &name, const QVariant &data)
	{
		theme->putToCache(name, data);
	}

	/**
	 * @brief loads content to cache
	 * @param map(cache_key => file in theme)
	 */
	void saveFilesToCache(const QVariantMap &map)
	{
		auto it = map.constBegin();
		while (it != map.constEnd()) {
			QByteArray ba = theme->loadData(it.value().toString());
			if (!ba.isNull()) {
				theme->putToCache(it.key(), QString::fromUtf8(ba));
			}
			++it;
		}
	}

	/**
	 * @brief That's about applying theme to certian session. So we register session's id in theme and allow theme
	 *        loader in theme's webview to init last parts of theme.
	 *        This is for cases when theme can't init itself fully w/o some knowledge about sesion.
	 * @param sessionId it's the same id as registered on internal web server
	 * @param props a list of proprties' names
	 * @return filled map prop=>value
	 */
	QVariantMap sessionProperties(const QString &sessionId, const QVariantList &props)
	{
		auto sess = _sessions.value(sessionId);
		QVariantMap ret;
		if (sess) {
			for (auto &p : props) {
				QString key = p.toString();
				ret.insert(key, sess->property(key.toUtf8().data()));
			}
		}
		return ret;
	}

	void setCaseInsensitiveFS(bool state = true)
	{
		theme->setCaseInsensitiveFS(state);
	}

	void setPrepareSessionHtml(bool enabled = true)
	{
		theme->cvtd->prepareSessionHtml = enabled;
	}

	void setSessionHtml(const QString &sessionId, const QString &html)
	{
		emit sessionHtmlReady(sessionId, html);
	}

	QVariantMap checkFilesExist(const QStringList &files, const QString baseDir = QString())
	{
		QVariantMap ret;
		QScopedPointer<Theme::ResourceLoader> loader(theme->resourceLoader());

		QString d(baseDir);
		if (!d.isEmpty()) {
			d += QLatin1Char('/');
		}
		foreach (const QString &f, files) {
			ret.insert(f, loader->fileExists(d + f));
		}

		return ret;
	}

	QString getFileContents(const QString &name) const
	{
		return QString(theme->loadData(name));
	}

	QString getFileContentsFromAdapterDir(const QString &name) const
	{
		QString adapterPath = theme->themeProvider()->themePath(QLatin1String("chatview/") + theme->id().split('/').first());
		QFile file(adapterPath + "/" + name);
		if (file.open(QIODevice::ReadOnly)) {
			QByteArray result = file.readAll();
			file.close();
			return QString::fromUtf8(result.constData(), result.size());
		} else {
			qDebug("Failed to open file %s: %s", qPrintable(file.fileName()),
				   qPrintable(file.errorString()));
		}
		return QString();
	}

	void setTransparent()
	{
		theme->setTransparentBackground(true);
	}
};







// JS Bridge object emedded by theme. Has any logic unrelted to contact itself
class ChatViewThemeJSUtil : public QObject {
	Q_OBJECT

	ChatViewTheme *theme;
	QString psiDefaultAvatarUrl;

	Q_PROPERTY(QString psiDefaultAvatarUrl MEMBER psiDefaultAvatarUrl CONSTANT)

public:
	ChatViewThemeJSUtil(ChatViewTheme *theme, QObject *parent = 0) :
	    QObject(parent),
	    theme(theme)
	{
		psiDefaultAvatarUrl = "psiglobal/avatar/default.png"; // relative to session url
		// may be in the future we can make different defaults. per transport for example
	}

	inline void putToCache(const QString &key, const QVariant &data)
	{
		theme->putToCache(key, data);
	}

public slots:
	QVariantMap loadFromCacheMulti(const QVariantList &list)
	{
		return theme->loadFromCacheMulti(list);
	}

	QVariant cache(const QString &name) const
	{
		return theme->cache(name);
	}


	QString psiOption(const QString &option) const
	{
		return JSUtil::variant2js(PsiOptions::instance()->getOption(option));
	}

	QString colorOption(const QString &option) const
	{
		return JSUtil::variant2js(ColorOpt::instance()->color(option));
	}

	QString formatDate(const QDateTime &dt, const QString &format) const
	{
		return dt.toLocalTime().toString(format);
	}

	QString strftime(const QDateTime &dt, const QString &format) const
	{
		char str[256];
		time_t t = dt.toTime_t();
		int s = ::strftime(str, 256, format.toLocal8Bit(), localtime(&t));
		if (s) {
			return QString::fromLocal8Bit(str, s);
		}
		return QString();
	}

	void console(const QString &text) const
	{
		qDebug("%s", qPrintable(text));
	}

	QString status2text(int status) const
	{
		return ::status2txt(status);
	}

	QString hex2rgba(const QString &hex, float opacity)
	{
		QColor color("#" + hex);
		color.setAlphaF(opacity);
		return QString("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green())
				.arg(color.blue()).arg(color.alpha());
	}
};


#ifndef QT_WEBENGINEWIDGETS_LIB
class SessionRequestHandler : public NAMDataHandler
{
	QSharedPointer<ChatViewThemeSession> session;

public:
	SessionRequestHandler(QSharedPointer<ChatViewThemeSession> &session) :
	    session(session) {}

	bool data(const QNetworkRequest &req, QByteArray &data, QByteArray &mime) const
	{
		Q_UNUSED(mime)
		data = session->theme.loadData(session->theme.cvtd->httpRelPath + req.url().path());
		if (!data.isNull()) {
			return true;
		}
		return false;
	}
};
#endif

//------------------------------------------------------------------------------
// ChatViewTheme
//------------------------------------------------------------------------------
ChatViewTheme::ChatViewTheme()
{

}

ChatViewTheme::ChatViewTheme(ChatViewThemeProvider *provider) :
	Theme(provider),
    cvtd(new ChatViewThemePrivate())
{
}

ChatViewTheme::ChatViewTheme(const ChatViewTheme &other) :
    Theme(other),
    cvtd(other.cvtd)
{

}

ChatViewTheme &ChatViewTheme::operator=(const ChatViewTheme &other)
{
	Theme::operator=(other);
	cvtd = other.cvtd;
	return *this;
}

ChatViewTheme::~ChatViewTheme()
{
}

bool ChatViewTheme::exists()
{
	if (id().isEmpty()) {
		return false;
	}
	ChatViewThemeProvider *provider = static_cast<ChatViewThemeProvider*>(themeProvider());
	QString tp = provider->themePath(QLatin1String("chatview/") + id());
	setFilePath(tp);
	return !tp.isEmpty();
}

/**
 * @brief Sets theme bridge, starts loading procedure from javascript adapter.
 * @param file full path to theme directory
 * @param helperScripts adapter.js and util.js
 * @param adapterPath path to directry with adapter
 * @return true on success
 */
bool ChatViewTheme::load(std::function<void(bool)> loadCallback)
{
	if (!exists()) {
		return false;
	}

	qDebug("Starting loading \"%s\" theme at \"%s\"", qPrintable(id()), qPrintable(filePath()));
	cvtd->loadCallback = loadCallback;
	if (cvtd->jsUtil.isNull())
		cvtd->jsLoader.reset(new ChatViewJSLoader(this));
		cvtd->jsUtil.reset(new ChatViewThemeJSUtil(this));
	if (cvtd->wv.isNull()) {
		cvtd->wv = new WebView(0);
	}

	QString themeType = id().section('/', 0, 0);
#if QT_WEBENGINEWIDGETS_LIB
	QWebChannel * channel = new QWebChannel(cvtd->wv->page());
	cvtd->wv->page()->setWebChannel(channel);
	channel->registerObject(QLatin1String("srvLoader"), cvtd->jsLoader.data());
	channel->registerObject(QLatin1String("srvUtil"), cvtd->jsUtil.data());

	//QString themeServer = ChatViewThemeProvider::serverAddr();
	cvtd->wv->page()->setHtml(QString(
	    "<html><head>\n"
	    "<script src=\"/psithemes/chatview/moment-with-locales.min.js\"></script>\n"
	    "<script src=\"/psithemes/chatview/util.js\"></script>\n"
	    "<script src=\"/psithemes/chatview/%1/adapter.js\"></script>\n"
	    "<script src=\"/psiglobal/qwebchannel.js\"></script>\n"
		"<script type=\"text/javascript\">\n"
			"document.addEventListener(\"DOMContentLoaded\", function () {\n"
				"new QWebChannel(qt.webChannelTransport, function (channel) {\n"
					"window.srvLoader = channel.objects.srvLoader;\n"
					"window.srvUtil = channel.objects.srvUtil;\n"
	                "initPsiTheme().adapter.loadTheme();\n"
				"});\n"
			"});\n"
		"</script></head></html>").arg(themeType), cvtd->jsLoader->serverUrl()
	);
	return true;
#else
	QStringList scriptPaths = QStringList()
	        << PsiThemeProvider::themePath(QLatin1String("chatview/moment-with-locales.min.js"))
	        << PsiThemeProvider::themePath(QLatin1String("chatview/util.js"))
	        << PsiThemeProvider::themePath(QLatin1String("chatview/") + themeType + QLatin1String("/adapter.js"));


	cvtd->wv->page()->mainFrame()->addToJavaScriptWindowObject("srvLoader", cvtd->jsLoader.data(), QT_JS_QTOWNERSHIP);
	cvtd->wv->page()->mainFrame()->addToJavaScriptWindowObject("srvUtil", cvtd->jsUtil.data(), QT_JS_QTOWNERSHIP);

	foreach (const QString &sp, scriptPaths) {
		cvtd->evaluateFromFile(sp, cvtd->wv->page()->mainFrame());
	}

	QString resStr = cvtd->wv->page()->mainFrame()->evaluateJavaScript(
				"try { initPsiTheme().adapter.loadTheme(); \"ok\"; } "
				"catch(e) { \"Error:\" + e + \"\\n\" + window.psiim.util.props(e); }").toString();

	if (resStr == "ok") {
		return true;
	}
	qWarning("javascript part of the theme loader "
			 "didn't return expected result: %s", qPrintable(resStr));
	return false;
#endif
}

bool ChatViewTheme::isMuc() const
{
	return dynamic_cast<GroupChatViewThemeProvider*>(themeProvider());
}

QByteArray ChatViewTheme::screenshot()
{
	return loadData("screenshot.png");
}


#if 0
QString ChatViewTheme::html(QObject *session)
{
	if (d->html.isEmpty()) {
#warning "Remove session from theme. That's at least weird"
#if 0
		if (session) {
			d->wv->page()->mainFrame()->addToJavaScriptWindowObject("chatSession", session);
		}
#endif
		return d->wv->evaluateJS(
					"try { psiim.adapter.getHtml(); } catch(e) { e.toString() + ' line:' +e.line; }").toString();
	}
	return d->html;
}
#endif

QVariantMap ChatViewTheme::loadFromCacheMulti(const QVariantList &list)
{
	QVariantMap ret;
	for (auto &item : list) {
		QString key = item.toString();
		ret[key] = cvtd->cache.value(key);
	}
	return ret;
}

void ChatViewTheme::putToCache(const QString &key, const QVariant &data)
{
	cvtd->cache.insert(key, data);
}

QVariant ChatViewTheme::cache(const QString &name) const
{
	return cvtd->cache.value(name);
}

void ChatViewTheme::setTransparentBackground(bool enabled)
{
	cvtd->transparentBackground = enabled;
}

bool ChatViewTheme::isTransparentBackground() const
{
	return cvtd->transparentBackground;
}

#ifndef QT_WEBENGINEWIDGETS_LIB
void ChatViewTheme::embedSessionJsObject(QSharedPointer<ChatViewThemeSession> session)
{
	QWebFrame *wf = session->webView()->page()->mainFrame();
	wf->addToJavaScriptWindowObject("srvUtil", new ChatViewThemeJSUtil(this, session->webView()));
	wf->addToJavaScriptWindowObject("srvSession", session->jsBridge());

	QStringList scriptPaths = QStringList()
	        << PsiThemeProvider::themePath(QLatin1String("chatview/moment-with-locales.min.js"))
	        << PsiThemeProvider::themePath(QLatin1String("chatview/util.js"))
	        << PsiThemeProvider::themePath(QLatin1String("chatview/") + id().section('/', 0, 0) + QLatin1String("/adapter.js"));

	foreach (const QString &script, scriptPaths) {
		cvtd->evaluateFromFile(script, wf);
	}
}
#endif

bool ChatViewTheme::applyToWebView(QSharedPointer<ChatViewThemeSession> session)
{
	session->theme = *this;

#if QT_WEBENGINEWIDGETS_LIB
	QWebEnginePage *page = session->webView()->page();
	if (cvtd->transparentBackground) {
		page->setBackgroundColor(Qt::transparent);
	}

	QWebChannel *channel = page->webChannel();
	if (!channel) {
		channel = new QWebChannel(session->webView());

		channel->registerObject(QLatin1String("srvUtil"), new ChatViewThemeJSUtil(this, session->webView()));
		channel->registerObject(QLatin1String("srvSession"), session->jsBridge());

		page->setWebChannel(channel);
		// channel is kept on F5 but all objects are cleared, so will be added later
	}

	ChatViewThemeProvider *provider = static_cast<ChatViewThemeProvider*>(themeProvider());
	page->profile()->setRequestInterceptor(provider->requestInterceptor());

	auto server = provider->themeServer();
	session->server = server;

	auto weakSession = session.toWeakRef();
	auto handler = [weakSession](qhttp::server::QHttpRequest* req, qhttp::server::QHttpResponse* res) -> bool
	{
		auto session = weakSession.lock();
		if (!session) {
			return false;
		}
		auto pair = session->getContents(req->url());
		if (pair.first.isNull()) {
			// not handled by chat. try handle by theme
			QString path = req->url().path(); // fully decoded
			if (path.isEmpty() || path == QLatin1String("/")) {
				res->setStatusCode(qhttp::ESTATUS_OK);
				res->headers().insert("Content-Type", "text/html;charset=utf-8");


				if (session->theme.cvtd->prepareSessionHtml) { // html should be prepared for ech individual session
					// Even crazier stuff starts here.
					// Basically we send to theme's webview instance a signal to
					// generate html for specific session. It does it async.
					// Then javascript sends a signal the html is ready,
					// indicating sessionId and html contents.
					// And only then we close the request with hot html.

					session->theme.cvtd->jsLoader->connect(session->theme.cvtd->jsLoader.data(),
					                        &ChatViewJSLoader::sessionHtmlReady,
					                        session->jsBridge(),
					[session, res](const QString &sessionId, const QString &html)
					{
						if (session->sessId == sessionId) {
							res->end(html.toUtf8()); // return html to client
							// and disconnect from loader
							session->theme.cvtd->jsLoader->disconnect(
							            session->theme.cvtd->jsLoader.data(),
							            &ChatViewJSLoader::sessionHtmlReady,
							            session->jsBridge(), nullptr);
							session->theme.cvtd->jsLoader->unregisterSession(session->sessId);
						}
					});
					session->theme.cvtd->jsLoader->registerSession(session);
					QString basePath = req->property("basePath").toString();
					session->theme.cvtd->wv->page()->runJavaScript(
					            QString(QLatin1String("psiim.adapter.generateSessionHtml(\"%1\", %2, \"%3\")"))
					            .arg(session->sessId, session->propsAsJsonString(), basePath));

				} else {
					res->end(session->theme.cvtd->html.toUtf8());
				}
				return true;
			} else {
				QByteArray data = session->theme.loadData(session->theme.cvtd->httpRelPath + path);
				if (!data.isNull()) {
					res->setStatusCode(qhttp::ESTATUS_OK);
					res->end(data);
					return true;
				}
			}
			return false;
		}
		res->setStatusCode(qhttp::ESTATUS_OK);
		res->headers().insert("Content-Type", pair.second);
		res->end(pair.first);
		return true;
		// there is a chance the windows is closed already when we come here..
	};
	session->sessId = server->registerSessionHandler(handler);
	QUrl url = server->serverUrl();
	QUrlQuery q;
	q.addQueryItem(QLatin1String("psiId"), session->sessId);
	url.setQuery(q);

	page->load(url);

	//QString id = provider->themeServer()->registerHandler(sessionObject);
	return true;
#else
	QWebPage *page = session->webView()->page();
	if (cvtd->transparentBackground) {
		QPalette palette;
		palette = session->webView()->palette();
		palette.setBrush(QPalette::Base, Qt::transparent);
		page->setPalette(palette);
		session->webView()->setAttribute(Qt::WA_OpaquePaintEvent, false);
	}

	page->setNetworkAccessManager(NetworkAccessManager::instance());

	SessionRequestHandler *handler = new SessionRequestHandler(session);
	session->sessId = NetworkAccessManager::instance()->registerSessionHandler(QSharedPointer<NAMDataHandler>(handler));

	QString html;
	if (cvtd->prepareSessionHtml) {
		QString basePath = "";
		cvtd->jsLoader->registerSession(session);
		html = cvtd->wv->page()->mainFrame()->evaluateJavaScript(
	            QString(QLatin1String("psiim.adapter.generateSessionHtml(\"%1\", %2, \"%3\")"))
	            .arg(session->sessId, session->propsAsJsonString(), basePath)).toString();
		cvtd->jsLoader->unregisterSession(session->sessId);
	} else {
		html = cvtd->html;
	}

	page->mainFrame()->setHtml(html, cvtd->jsLoader->serverUrl());

	return true;
#endif
}

ChatViewThemeSession::~ChatViewThemeSession()
{
#ifdef QT_WEBENGINEWIDGETS_LIB
	if (server) {
		server->unregisterSessionHandler(sessId);
	}
#else
	NetworkAccessManager::instance()->unregisterSessionHandler(sessId);
#endif
}

#include "chatviewtheme.moc"

