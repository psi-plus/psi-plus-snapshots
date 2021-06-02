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
    gstEventLoopThread.setObjectName("GstEventLoop");

    auto resourcePath = params.value("resourcePath").toString();
    gstEventLoop      = new GstMainLoop(resourcePath);
    deviceMonitor     = new DeviceMonitor(gstEventLoop);
    gstEventLoop->moveToThread(&gstEventLoopThread);

    connect(
        &gstEventLoopThread, &QThread::started, gstEventLoop,
        [this]() {
            Q_ASSERT(QThread::currentThread() == &gstEventLoopThread);
            // connect(&gstEventLoopThread, &QThread::finished, gstEventLoop, &QObject::deleteLater);
            connect(gstEventLoop, &GstMainLoop::started, this, &GstProvider::initialized, Qt::QueuedConnection);
            // do any custom stuff here before glib event loop started. it's already initialized
            if (!gstEventLoop->start()) {
                qWarning("glib event loop failed to initialize");
                gstEventLoopThread.exit(1); // noop if ~GstProvider() was called first?
                return;
            }
        },
        Qt::QueuedConnection);
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

bool GstProvider::init()
{
    gstEventLoopThread.start();
    return true;
}

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

RtpSessionContext *GstProvider::createRtpSession() { return new GstRtpSessionContext(gstEventLoop); }

AudioRecorderContext *GstProvider::createAudioRecorder() { return new GstAudioRecorderContext(gstEventLoop); }

}
