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

#include "gstrtpchannel.h"

#include "gstrtpsessioncontext.h"

//----------------------------------------------------------------------------
// GstRtpChannel
//----------------------------------------------------------------------------
// for a live transmission we really shouldn't have excessive queuing (or
//   *any* queuing!), so we'll cap the queue sizes.  if the system gets
//   overloaded and the thread scheduling skews such that our queues get
//   filled before they can be emptied, then we'll start dropping old
//   items making room for new ones.  on a live transmission there's no
//   sense in keeping ancient data around.  we just drop and move on.
#define QUEUE_PACKET_MAX 25

// don't wake the main thread more often than this, for performance reasons
#define WAKE_PACKET_MIN 40

namespace PsiMedia {

GstRtpChannel::GstRtpChannel() { }

QObject *GstRtpChannel::qobject() { return this; }

void GstRtpChannel::setEnabled(bool b)
{
    QMutexLocker locker(&m);
    enabled = b;
}

int GstRtpChannel::packetsAvailable() const { return in.count(); }

PRtpPacket GstRtpChannel::read() { return in.takeFirst(); }

void GstRtpChannel::receiver_push_packet_for_write(const PRtpPacket &rtp)
{
    if (session)
        session->push_packet_for_write(this, rtp);
}

void GstRtpChannel::write(const PRtpPacket &rtp)
{
    m.lock();
    if (!enabled)
        return;
    m.unlock();

    receiver_push_packet_for_write(rtp);
    ++written_pending;

    // only queue one call per eventloop pass
    if (written_pending == 1)
        QMetaObject::invokeMethod(this, "processOut", Qt::QueuedConnection);
}

void GstRtpChannel::push_packet_for_read(const PRtpPacket &rtp)
{
    QMutexLocker locker(&m);
    if (!enabled)
        return;

    // if the queue is full, bump off the oldest to make room
    if (pending_in.count() >= QUEUE_PACKET_MAX)
        pending_in.removeFirst();

    pending_in += rtp;

    // TODO: use WAKE_PACKET_MIN and wake_time ?

    if (!wake_pending) {
        wake_pending = true;
        QMetaObject::invokeMethod(this, "processIn", Qt::QueuedConnection);
    }
}

void GstRtpChannel::processIn()
{
    int oldcount = in.count();

    m.lock();
    wake_pending = false;
    in += pending_in;
    pending_in.clear();
    m.unlock();

    if (in.count() > oldcount)
        emit readyRead();
}

void GstRtpChannel::processOut()
{
    int count       = written_pending;
    written_pending = 0;
    emit packetsWritten(count);
}

} // namespace PsiMedia
