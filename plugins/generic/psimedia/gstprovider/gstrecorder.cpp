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

#include "gstrecorder.h"

#include "rwcontrol.h"

#include <QIODevice>

namespace PsiMedia {

GstRecorder::GstRecorder(QObject *parent) :
    QObject(parent), control(nullptr), recordDevice(nullptr), nextRecordDevice(nullptr), record_cancel(false),
    wake_pending(false)
{
}

void GstRecorder::setDevice(QIODevice *dev)
{
    Q_ASSERT(!recordDevice);
    Q_ASSERT(!nextRecordDevice);

    if (control) {
        recordDevice = dev;

        RwControlRecord record;
        record.enabled = true;
        control->setRecord(record);
    } else {
        // queue up the device for later
        nextRecordDevice = dev;
    }
}

void GstRecorder::stop()
{
    Q_ASSERT(recordDevice || nextRecordDevice);
    Q_ASSERT(!record_cancel);

    if (nextRecordDevice) {
        // if there was only a queued device, then there's
        //   nothing to do but dequeue it
        nextRecordDevice = nullptr;
    } else {
        record_cancel = true;

        RwControlRecord record;
        record.enabled = false;
        control->setRecord(record);
    }
}

void GstRecorder::startNext()
{
    if (control && !recordDevice && nextRecordDevice) {
        recordDevice     = nextRecordDevice;
        nextRecordDevice = nullptr;

        RwControlRecord record;
        record.enabled = true;
        control->setRecord(record);
    }
}

void GstRecorder::push_data_for_read(const QByteArray &buf)
{
    QMutexLocker locker(&m);
    pending_in += buf;
    if (!wake_pending) {
        wake_pending = true;
        QMetaObject::invokeMethod(this, "processIn", Qt::QueuedConnection);
    }
}

void GstRecorder::processIn()
{
    m.lock();
    wake_pending         = false;
    QList<QByteArray> in = pending_in;
    pending_in.clear();
    m.unlock();

    QPointer<QObject> self = this;

    while (!in.isEmpty()) {
        QByteArray buf = in.takeFirst();

        if (!buf.isEmpty()) {
            recordDevice->write(buf);
        } else // EOF
        {
            recordDevice->close();
            recordDevice = nullptr;

            bool wasCancelled = record_cancel;
            record_cancel     = false;

            if (wasCancelled) {
                emit stopped();
                if (!self)
                    return;
            }
        }
    }
}

} // namespace PsiMedia
