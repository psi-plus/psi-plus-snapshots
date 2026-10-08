// SPDX-License-Identifier: LGPL-2.1-or-later

#include "icelocaltransport.h"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QPointer>
#include <QTimer>
#include <QUdpSocket>

using namespace XMPP;

static bool startTransport(IceLocalTransport &transport, QUdpSocket *socket, bool takeOwnership)
{
    bool       started = false;
    QEventLoop loop;
    QObject::connect(&transport, &IceTransport::started, &loop, [&]() {
        started = true;
        loop.quit();
    });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    transport.start(socket, takeOwnership);
    loop.exec();
    return started;
}

static void drainDeferredDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    auto *owned = new QUdpSocket;
    if (!owned->bind(QHostAddress::LocalHost, 0))
        qFatal("failed to bind owned socket");
    QPointer<QUdpSocket> ownedGuard(owned);
    {
        IceLocalTransport transport;
        if (!startTransport(transport, owned, true))
            qFatal("owned socket transport did not start");
    }
    drainDeferredDeletes();
    if (ownedGuard)
        qFatal("IceLocalTransport leaked an owned QUdpSocket");

    auto *borrowed = new QUdpSocket;
    if (!borrowed->bind(QHostAddress::LocalHost, 0))
        qFatal("failed to bind borrowed socket");
    QPointer<QUdpSocket> borrowedGuard(borrowed);
    {
        IceLocalTransport transport;
        if (!startTransport(transport, borrowed, false))
            qFatal("borrowed socket transport did not start");
    }
    drainDeferredDeletes();
    if (!borrowedGuard)
        qFatal("IceLocalTransport destroyed a borrowed QUdpSocket");
    delete borrowed;

    return 0;
}
