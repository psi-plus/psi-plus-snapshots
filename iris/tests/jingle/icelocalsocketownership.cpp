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
    if (!owned->bind(QHostAddress(QHostAddress::LocalHost), 0))
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
    if (!borrowed->bind(QHostAddress(QHostAddress::LocalHost), 0))
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

    // Cancellation before deferred startup must complete without starting or leaking the socket.
    for (bool takeOwnership : { false, true }) {
        auto                *socket = new QUdpSocket;
        QPointer<QUdpSocket> guard(socket);
        bool                 started = false;
        bool                 stopped = false;
        {
            IceLocalTransport transport;
            QEventLoop        loop;
            QObject::connect(&transport, &IceTransport::started, &loop, [&]() { started = true; });
            QObject::connect(&transport, &IceTransport::stopped, &loop, [&]() {
                stopped = true;
                loop.quit();
            });
            QTimer::singleShot(1000, &loop, &QEventLoop::quit);
            transport.start(socket, takeOwnership);
            transport.stop();
            loop.exec();
            if (started || !stopped)
                qFatal("cancellation before socket startup did not finish cleanly");
        }
        drainDeferredDeletes();
        if (takeOwnership && guard)
            qFatal("cancellation leaked an owned socket");
        if (!takeOwnership && !guard)
            qFatal("cancellation destroyed a borrowed socket");
        if (guard)
            delete socket;
    }

    return 0;
}
