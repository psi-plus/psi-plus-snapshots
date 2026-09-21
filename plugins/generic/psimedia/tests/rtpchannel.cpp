/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "gstrtpchannel.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    PsiMedia::GstRtpChannel channel;
    PsiMedia::PRtpPacket    packet;
    packet.type     = PsiMedia::PRtpPacket::Type::Rtp;
    packet.rawValue = QByteArray::fromHex("806f0001000003c0102030407f");

    channel.setEnabled(false);
    channel.write(packet);

    if (!channel.m.tryLock()) {
        qCritical() << "disabled RTP write left the channel mutex locked";
        return 1;
    }
    channel.m.unlock();

    int written = 0;
    QObject::connect(&channel, &PsiMedia::GstRtpChannel::packetsWritten, [&](int count) { written += count; });

    channel.setEnabled(true);
    channel.write(packet);
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    if (written != 1 || channel.written_pending != 0) {
        qCritical() << "RTP channel did not recover after a disabled write" << written << channel.written_pending;
        return 2;
    }

    qInfo() << "RTP channel disabled-write regression passed";
    return 0;
}
