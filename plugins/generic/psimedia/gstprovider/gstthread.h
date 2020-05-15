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

#ifndef PSI_GSTTHREAD_H
#define PSI_GSTTHREAD_H

#include <QObject>
#include <QString>
#include <functional>
#include <glib.h>

namespace PsiMedia {

// this class is kind of like QCA::SyncThread but for glib.  It atomically
//   starts up a thread, initializes gstreamer, and sets up a glib eventloop
//   ready for use.  if you want to do stuff in the other thread, set
//   up a glib timeout of 0 against mainContext(), and go from there.

class GstMainLoop : public QObject {
    Q_OBJECT

public:
    typedef std::function<void(void *userData)> ContextCallback;

    explicit GstMainLoop(const QString &resPath);
    ~GstMainLoop() override;

    QString       gstVersion() const;
    GMainContext *mainContext();
    bool          isInitialized() const;
    bool          execInContext(const ContextCallback &cb, void *userData);
    bool          start();

signals:
    void started();

public slots:
    void stop();

private:
    class Private;
    Private *d;
};

}

#endif
