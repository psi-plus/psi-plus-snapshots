/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 * Copyright (C) 2020  Psi IM Team
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

#include "psimediaprovider.h"

#include "devices.h"
#include "gstaudiorecordercontext.h"
#include "gstfeaturescontext.h"
#include "gstprovider.h"
#include "gstrtpsessioncontext.h"
#include "gstthread.h"

#include <QtPlugin>

namespace PsiMedia {

//----------------------------------------------------------------------------
// GstProvider
//----------------------------------------------------------------------------
GstProvider::GstProvider(const QVariantMap &params)
{
    // qDebug("GstProvider::GstProvider thread=%p", QThread::currentThreadId());
    gstEventLoopThread.setObjectName("GstEventLoop");

    auto resourcePath = params.value("resourcePath").toString();
    gstEventLoop      = new GstMainLoop(resourcePath);
    deviceMonitor     = new DeviceMonitor(gstEventLoop);
    gstEventLoop->moveToThread(&gstEventLoopThread);

    QMutex waitMutex;
    waitMutex.lock();
    QWaitCondition   wait;
    std::atomic_bool success { false };
    connect(
        &gstEventLoopThread, &QThread::started, gstEventLoop,
        [this, &wait, &success]() {
            Q_ASSERT(QThread::currentThread() == &gstEventLoopThread);
            // connect(&gstEventLoopThread, &QThread::finished, gstEventLoop, &QObject::deleteLater);
            connect(gstEventLoop, &GstMainLoop::started, [&wait, &success]() {
                // faired by timer from gst loop. means complete success in starting.
                success.store(true);
                wait.wakeOne();
            });
            // do any custom stuff here before glib event loop started. it's already initialized
            if (!gstEventLoop->start()) { // this call won't return while event loop is still running
                qWarning("glib event loop failed to initialize");
                gstEventLoopThread.exit(1); // noop if ~GstProvider() was called first?
                wait.wakeOne();
                return;
            }
        },
        Qt::QueuedConnection);
    gstEventLoopThread.start();
    wait.wait(&waitMutex);
    waitMutex.unlock();
    if (!success.load()) {
        gstEventLoopThread.wait();
        delete gstEventLoop; // will null it coz QPointer
    }
}

GstProvider::~GstProvider()
{
    if (gstEventLoopThread.isRunning()) {
        gstEventLoop->stop();      // stop glib event loop
        gstEventLoopThread.quit(); // stop qt even loop in its thread
        gstEventLoopThread.wait(); // wait till everything is eventually stopped
        delete gstEventLoop;
    }
}

QObject *GstProvider::qobject() { return this; }

bool GstProvider::isInitialized() const { return gstEventLoop && gstEventLoop->isInitialized(); }

QString GstProvider::creditName() const { return "GStreamer"; }

QString GstProvider::creditText() const
{
    QString str = QString("This application uses GStreamer %1, a comprehensive "
                          "open-source and cross-platform multimedia framework.  For "
                          "more information, see http://www.gstreamer.net/\n\n"
                          "If you enjoy this software, please give the GStreamer "
                          "people a million dollars.")
                      .arg(gstEventLoop->gstVersion());
    return str;
}

FeaturesContext *GstProvider::createFeatures() { return new GstFeaturesContext(gstEventLoop, deviceMonitor); }

RtpSessionContext *GstProvider::createRtpSession() { return new GstRtpSessionContext(gstEventLoop, deviceMonitor); }

AudioRecorderContext *GstProvider::createAudioRecorder() { return new GstAudioRecorderContext(gstEventLoop); }

}
