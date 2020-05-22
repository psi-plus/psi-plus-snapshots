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

#ifndef PSIMEDIA_GSTRTPCHANNEL_H
#define PSIMEDIA_GSTRTPCHANNEL_H

#include "psimediaprovider.h"

#include <QMutex>
#include <QObject>

namespace PsiMedia {

class GstRtpSessionContext;

class GstRtpChannel : public QObject, public RtpChannelContext {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::RtpChannelContext)

public:
    bool                  enabled;
    QMutex                m;
    GstRtpSessionContext *session;
    QList<PRtpPacket>     in;

    // QTime wake_time;
    bool              wake_pending;
    QList<PRtpPacket> pending_in;

    int written_pending;

    GstRtpChannel();

    virtual QObject *qobject();

    virtual void setEnabled(bool b);

    virtual int packetsAvailable() const;

    virtual PRtpPacket read();

    virtual void write(const PRtpPacket &rtp);

    // session calls this, which may be in another thread
    void push_packet_for_read(const PRtpPacket &rtp);

Q_SIGNALS:
    void readyRead();
    void packetsWritten(int count);

private Q_SLOTS:
    void processIn();

    void processOut();

private:
    void receiver_push_packet_for_write(const PRtpPacket &rtp);
};
} // namespace PsiMedia

#endif // PSIMEDIA_GSTRTPCHANNEL_H
