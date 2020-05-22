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

#ifndef PSIMEDIA_GSTRECORDER_H
#define PSIMEDIA_GSTRECORDER_H

#include <QMutex>
#include <QPointer>

class QIODevice;

namespace PsiMedia {

class RwControlLocal;

//----------------------------------------------------------------------------
// GstRecorder
//----------------------------------------------------------------------------
class GstRecorder : public QObject {
    Q_OBJECT

public:
    RwControlLocal *control;
    QIODevice *     recordDevice, *nextRecordDevice;
    bool            record_cancel;

    QMutex            m;
    bool              wake_pending;
    QList<QByteArray> pending_in;

    explicit GstRecorder(QObject *parent = nullptr);

    void setDevice(QIODevice *dev);
    void stop();
    void startNext();

    // session calls this, which may be in another thread
    void push_data_for_read(const QByteArray &buf);

signals:
    void stopped();

private slots:
    void processIn();
};

} // namespace PsiMedia

#endif // PSIMEDIA_GSTRECORDER_H
