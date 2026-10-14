// SPDX-License-Identifier: LGPL-2.1-or-later
// SXE payload on an ICE SCTP data channel between two clients, and the fallback to SXE messages
// when the channel breaks. Reuses the looped-back two-client fixture; ICE runs on localhost.
#define main iris_sxesession_main
#include "sxesession.cpp"
#undef main

#include <iris/jingle-ft.h>
#include <iris/jingle-ice.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_hash.h>

#include <QBuffer>
#include <QElapsedTimer>
#include <QThread>

#include <functional>

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-sxe-connection"));
    disco.setFeatures(Features(features));
    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

// Exchanges stanzas and lets the localhost ICE and SCTP traffic run until done() or the timeout.
static bool pumpUntil(Endpoint &a, Endpoint &b, const std::function<bool()> &done, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        pump(a, b);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    pump(a, b);
    return done();
}

static int sxeMessages()
{
    int count = 0;
    for (const auto &message : std::as_const(sentMessages))
        if (!message.firstChildElement(QStringLiteral("sxe")).isNull())
            ++count;
    return count;
}

static J::Connection::Ptr dataChannel(SXE::Application *application)
{
    if (!application->transport())
        return {};
    for (const auto &channel : application->transport()->channels())
        if (channel && channel->features() & J::TransportFeature::MessageOriented
            && channel->features() & J::TransportFeature::DataOriented)
            return channel;
    return {};
}

static bool synchronized(SXE::Application *a, SXE::Application *b)
{
    return a && b && a->syncState() == SXE::Application::SyncState::Synchronized
        && b->syncState() == SXE::Application::SyncState::Synchronized;
}

static bool bundled(J::Session *session, const QString &a, const QString &b)
{
    for (const auto &group : session->negotiatedGroupings())
        if (group.semantics == QLatin1String("BUNDLE") && group.contents.contains(a) && group.contents.contains(b))
            return true;
    return false;
}
