/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "gstthread.h"

#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QLibrary>
#include <QMutex>
#include <QQueue>
#include <QSemaphore>
#include <QStringList>
#include <QWaitCondition>
#include <gst/gst.h>

#include <atomic>

namespace PsiMedia {

//----------------------------------------------------------------------------
// GstSession
//----------------------------------------------------------------------------
// converts Qt-ified commandline args back to C style
class CArgs {
public:
    int    argc;
    char **argv;

    CArgs()
    {
        argc  = 0;
        argv  = nullptr;
        count = 0;
        data  = nullptr;
    }

    ~CArgs()
    {
        if (count > 0) {
            for (int n = 0; n < count; ++n)
                delete[] data[n];
            free(argv);
            free(data);
        }
    }

    void set(const QStringList &args)
    {
        count = args.count();
        if (count == 0) {
            data = nullptr;
            argc = 0;
            argv = nullptr;
        } else {
            data = static_cast<char **>(malloc(sizeof(char *) * quintptr(count)));
            argv = static_cast<char **>(malloc(sizeof(char *) * quintptr(count)));
            for (int n = 0; n < count; ++n) {
                QByteArray cs = args[n].toLocal8Bit();
                data[n]       = static_cast<char *>(qstrdup(cs.data()));
                argv[n]       = data[n];
            }
            argc = count;
        }
    }

private:
    int    count;
    char **data;
};

/*static void loadPlugins(const QString &pluginPath, bool print = false)
{
    if (print)
        qDebug("Loading plugins in [%s]", qPrintable(pluginPath));
    QDir        dir(pluginPath);
    QStringList entryList = dir.entryList(QDir::Files);
    foreach (QString entry, entryList) {
        if (!QLibrary::isLibrary(entry))
            continue;
        QString    filePath = dir.filePath(entry);
        GError *   err      = nullptr;
        GstPlugin *plugin   = gst_plugin_load_file(filePath.toUtf8().data(), &err);
        if (!plugin) {
            if (print) {
                qDebug("**FAIL**: %s: %s", qPrintable(entry), err->message);
            }
            g_error_free(err);
            continue;
        }
        if (print) {
            qDebug("   OK   : %s name=[%s]", qPrintable(entry), gst_plugin_get_name(plugin));
        }
        gst_object_unref(plugin);
    }

    if (print)
        qDebug("");
}*/

static int compare_gst_version(uint a1, uint a2, uint a3, uint b1, uint b2, uint b3)
{
    if (a1 > b1)
        return 1;
    else if (a1 < b1)
        return -1;

    if (a2 > b2)
        return 1;
    else if (a2 < b2)
        return -1;

    if (a3 > b3)
        return 1;
    else if (a3 < b3)
        return -1;

    return 0;
}

class GstSession {
public:
    CArgs   args;
    QString version;
    bool    success;

    explicit GstSession(const QString &pluginPath = QString())
    {
        args.set(QCoreApplication::instance()->arguments());

        // ignore "system" plugins
        if (!qEnvironmentVariableIsSet("GST_PLUGIN_SYSTEM_PATH") && !pluginPath.isEmpty()) {
            qputenv("GST_PLUGIN_SYSTEM_PATH", pluginPath.toLocal8Bit());
        }

        // you can also use NULLs here if you don't want to pass args
        GError *error;
        if (!gst_init_check(&args.argc, &args.argv, &error)) {
            success = false;
            return;
        }

        guint major, minor, micro, nano;
        gst_version(&major, &minor, &micro, &nano);

        QString nano_str;
        if (nano == 1)
            nano_str = " (CVS)";
        else if (nano == 2)
            nano_str = " (Prerelease)";

        version = QString("%1.%2.%3%4")
                      .arg(major)
                      .arg(minor)
                      .arg(micro)
                      .arg(!nano_str.isEmpty() ? qPrintable(nano_str) : "");

        uint need_maj = 1;
        uint need_min = 4;
        uint need_mic = 0;
        if (compare_gst_version(major, minor, micro, need_maj, need_min, need_mic) < 0) {
            qDebug("Need GStreamer version %d.%d.%d", need_maj, need_min, need_mic);
            success = false;
            return;
        }

        // manually load plugins?
        // if(!pluginPath.isEmpty())
        //    loadPlugins(pluginPath);

        // gstcustomelements_register();
        // gstelements_register();

        QStringList reqelem
            = { "opusenc",         "opusdec",      "vorbisenc",    "vorbisdec",      "theoraenc",    "theoradec",
                "rtpopuspay",      "rtpopusdepay", "rtpvorbispay", "rtpvorbisdepay", "rtptheorapay", "rtptheoradepay",
                "filesrc",         "decodebin",    "jpegdec",      "oggmux",         "oggdemux",     "audioconvert",
                "audioresample",   "volume",       "level",        "videoconvert",   "videorate",    "videoscale",
                "rtpjitterbuffer", "audiomixer",   "appsink" };
#ifndef Q_OS_WIN
        reqelem << "webrtcechoprobe";
#endif

#if defined(Q_OS_MAC)
        reqelem << "osxaudiosrc"
                << "osxaudiosink";
#ifdef HAVE_OSXVIDIO
        reqelem << "osxvideosrc";
#endif
#elif defined(Q_OS_LINUX)
        reqelem << "v4l2src";
#elif defined(Q_OS_UNIX)
        reqelem << "osssrc"
                << "osssink";
#elif defined(Q_OS_WIN)
        reqelem << "directsoundsrc"
                << "directsoundsink"
                << "ksvideosrc";
#endif

        foreach (const QString &name, reqelem) {
            GstElement *e = gst_element_factory_make(name.toLatin1().data(), nullptr);
            if (!e) {
                qDebug("Unable to load element '%s'.", qPrintable(name));
                success = false;
                return;
            }

            g_object_unref(G_OBJECT(e));
        }

        success = true;
    }

    ~GstSession()
    {
        // docs say to not bother with gst_deinit, but we'll do it
        //   anyway in case there's an issue with plugin unloading
        // update: there could be other gstreamer users, so we
        //   probably shouldn't call this.  also, it appears to crash
        //   on mac for at least one user..   maybe the function is
        //   not very well tested.
        // gst_deinit();
    }
};

//----------------------------------------------------------------------------
// GstMainLoop
//----------------------------------------------------------------------------

class GstMainLoop::Private {
public:
    typedef struct {
        GSource               parent;
        GstMainLoop::Private *d = nullptr;
    } BridgeQueueSource;

    GstMainLoop *                                       q = nullptr;
    QString                                             pluginPath;
    GstSession *                                        gstSession = nullptr;
    std::atomic_bool                                    success;
    std::atomic_bool                                    stopping;
    GMainContext *                                      mainContext = nullptr;
    GMainLoop *                                         mainLoop    = nullptr;
    QMutex                                              queueMutex;
    QMutex                                              stateMutex;
    QWaitCondition                                      waitCond;
    BridgeQueueSource *                                 bridgeSource = nullptr;
    guint                                               bridgeId     = 0;
    QQueue<QPair<GstMainLoop::ContextCallback, void *>> bridgeQueue;

    Private(GstMainLoop *q) : q(q), success(false), stopping(false) { }

    static gboolean cb_loop_started(gpointer data) { return static_cast<Private *>(data)->loop_started(); }

    gboolean loop_started()
    {
        success = true;
        emit q->started();
        stateMutex.unlock();
        return FALSE;
    }

    static gboolean bridge_callback(gpointer data)
    {
        auto d = static_cast<GstMainLoop::Private *>(data);
        while (!d->bridgeQueue.empty()) {
            d->queueMutex.lock();
            QPair<GstMainLoop::ContextCallback, void *> p;
            bool                                        exist = !d->bridgeQueue.empty();
            if (exist)
                p = d->bridgeQueue.dequeue();
            d->queueMutex.unlock();
            bool stopping = d->stopping.load();
            if (exist)
                p.first(p.second);
            if (stopping) // REVIEW if it's possible to not have anything else at all on the queue during stop
                return FALSE;
        }

        return d->mainLoop == nullptr ? FALSE : TRUE;
    }

    static gboolean bridge_prepare(GSource *source, gint *timeout_)
    {
        *timeout_      = -1;
        auto         d = reinterpret_cast<Private::BridgeQueueSource *>(source)->d;
        QMutexLocker locker(&d->queueMutex);
        return !d->bridgeQueue.empty() ? TRUE : FALSE;
    }

    static gboolean bridge_check(GSource *source)
    {
        auto         d = reinterpret_cast<Private::BridgeQueueSource *>(source)->d;
        QMutexLocker locker(&d->queueMutex);
        return !d->bridgeQueue.empty() ? TRUE : FALSE;
    }

    static gboolean bridge_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
    {
        Q_UNUSED(source)
        if (callback(user_data))
            return TRUE;
        else
            return FALSE;
    }
};

GstMainLoop::GstMainLoop(const QString &resPath) : QObject()
{
    d             = new Private(this);
    d->pluginPath = resPath;

    // create a variable of type GSourceFuncs
    static GSourceFuncs bridgeFuncs
        = { Private::bridge_prepare, Private::bridge_check, Private::bridge_dispatch, nullptr, nullptr, nullptr };

    // create a new source
    d->bridgeSource = reinterpret_cast<Private::BridgeQueueSource *>(
        g_source_new(&bridgeFuncs, sizeof(Private::BridgeQueueSource)));
    d->bridgeSource->d = d;

    // HACK: if gstreamer initializes before certain Qt internal
    //   initialization occurs, then the app becomes unstable.
    //   I don't know what exactly needs to happen, or where the
    //   bug is, but if I fiddle with the default QStyle before
    //   initializing gstreamer, then this seems to solve it.
    //   it could be a bug in QCleanlooksStyle or QGtkStyle, which
    //   may conflict with separate Gtk initialization that may
    //   occur through gstreamer plugin loading.
    //{
    //    QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxCritical, 0, 0);
    //}
}

GstMainLoop::~GstMainLoop()
{
    stop();
    g_source_unref(&d->bridgeSource->parent);
    delete d;
}

void GstMainLoop::stop()
{
    // executed only in thread of gstprovider (not in gst event loop thread)

    d->stateMutex.lock(); // if we are still starting then it will lock till success or failure
    d->stopping = true;
    // with locked mutex we come here even after complete or otherwise we don't need to deinit anything
    if (d->success.exchange(false)) {
        QSemaphore stopSem;
        bool       stopped = execInContext(
            [this, &stopSem](void *) {
                g_main_loop_quit(d->mainLoop);
                qDebug("g_main_loop_quit");
                stopSem.release(1);
            },
            this);

        if (stopped) // if stop event really was scheduled to glib main loop.
            stopSem.acquire(1);

        qDebug("GstMainLoop::stop() finished");
    }
    d->stateMutex.unlock();
}

QString GstMainLoop::gstVersion() const { return d->gstSession->version; }

GMainContext *GstMainLoop::mainContext() { return d->mainContext; }

bool GstMainLoop::isInitialized() const { return d->success; }

bool GstMainLoop::execInContext(const ContextCallback &cb, void *userData)
{
    if (d->mainLoop) {
        QMutexLocker(&d->queueMutex);
        d->bridgeQueue.enqueue({ cb, userData });
        g_main_context_wakeup(d->mainContext);
        return true;
    }
    return false;
}

bool GstMainLoop::start()
{
    qDebug("GStreamer thread started");

    // this will be unlocked as soon as the mainloop runs
    d->stateMutex.lock();
    if (d->stopping) { // seem stop() was caller right after start
        d->stateMutex.unlock();
        return false;
    }

    d->gstSession = new GstSession(d->pluginPath);

    // report error
    if (!d->gstSession->success) {
        d->success = false;
        delete d->gstSession;
        d->gstSession = nullptr;
        qWarning("GStreamer thread completed (error)");
        d->stateMutex.unlock();
        return false;
    }

    // qDebug("Using GStreamer version %s", qPrintable(d->gstSession->version));

    d->mainContext = g_main_context_ref_thread_default();
    d->mainLoop    = g_main_loop_new(d->mainContext, FALSE);

    // attach bridge source to context
    d->bridgeId = g_source_attach(&d->bridgeSource->parent, d->mainContext);
    g_source_set_callback(&d->bridgeSource->parent, GstMainLoop::Private::bridge_callback, d, nullptr);

    // deferred call to loop_started()
    GSource *timer = g_timeout_source_new(0);
    g_source_attach(timer, d->mainContext);
    g_source_set_callback(timer, GstMainLoop::Private::cb_loop_started, d, nullptr);
    // d->stateMutex.unlock();

    qDebug("kick off glib event loop");
    // kick off the event loop
    g_main_loop_run(d->mainLoop);

    g_source_destroy(timer);
    g_source_unref(timer);

    g_main_loop_unref(d->mainLoop);
    d->mainLoop = nullptr;
    g_main_context_unref(d->mainContext);
    d->mainContext = nullptr;
    delete d->gstSession;
    d->gstSession = nullptr;

    return true;
}

}
