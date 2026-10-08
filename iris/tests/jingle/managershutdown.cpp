// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/s5b.h"
#include <iris/jingle-ft.h>
#include <iris/jingle-s5b.h>
#include <iris/jingle-session.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp_client.h>

#include <QCoreApplication>
#include <QPointer>
#include <QtCrypto>
#include <cstdio>

using namespace XMPP;
namespace J  = XMPP::Jingle;
namespace FT = J::FileTransfer;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

enum class Tail { Unregistered, Active, Deferred, Unattached };

static void testClientDestruction(Tail tail)
{
    TcpPortReserver reserver;
    auto           *client = new Client;
    client->setTcpPortReserver(&reserver);
    auto                *manager = client->jingleManager();
    QPointer<J::Manager> managerGuard(manager);
    auto                *session = manager->newSession(Jid(QStringLiteral("peer@example.test/device")));
    QPointer<J::Session> sessionGuard(session);
    check(session->parent() == manager, "Unregistered session has no manager lifetime owner");
    auto                    *content = session->newContent(FT::NS, J::Origin::Initiator);
    QPointer<J::Application> contentGuard(content);
    check(content, "File transfer content was not created");
    auto transport = session->newOutgoingTransport(J::S5B::NS);
    check(bool(transport) && content->setTransport(transport), "S5B transport was not assigned");
    QPointer<J::TransportManager> transportManager(transport->pad()->manager());
    QWeakPointer<J::Transport>    transportGuard(transport);
    transport.clear(); // Only the application/selector owns the actual transport.
    if (tail != Tail::Unattached)
        session->addContent(content);
    if (tail == Tail::Active || tail == Tail::Deferred) {
        const auto sid = manager->registerSession(session);
        check(!sid.isEmpty(), "Fixture SID was not registered");
        content->setState(J::State::Active);
    }
    int terminated = 0;
    int destroyed  = 0;
    QObject::connect(session, &J::Session::terminated, QCoreApplication::instance(), [&] {
        ++terminated;
        check(!session->newContent(FT::NS) && !session->newOutgoingTransport(J::S5B::NS),
              "Terminal session created new application/transport");
        check(manager->registerSession(session).isEmpty(), "Terminal session was registered again");
        check(contentGuard && contentGuard->state() == J::State::Finished,
              "Session terminated before application was stopped");
        check(transportManager && client->s5bManager() && client->ibbManager(),
              "Application stopped after transport infrastructure destruction");
    });
    QObject::connect(content, &J::Application::destroying, QCoreApplication::instance(), [&] {
        ++destroyed;
        check(managerGuard && transportManager && client->s5bManager() && client->ibbManager(),
              "Application destroyed after its infrastructure");
    });
    if (tail == Tail::Deferred) {
        client->close(true);
        check(sessionGuard && contentGuard, "Deferred fixture was already deleted");
        check(session->contentList().isEmpty(), "Finished session retained signaling contents");
        check(!manager->session(session->peer(), session->sid()), "Finished session still routes incoming IQs");
    }
    delete client; // No processEvents()/sendPostedEvents() may be needed for teardown.
    check(terminated == 1 && destroyed == 1, "Session termination/destruction was skipped or repeated");
    check(!sessionGuard && !contentGuard && transportGuard.isNull(),
          "Session/application/transport outlived Client destruction");
    check(!managerGuard && !transportManager, "Infrastructure was not destroyed");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static void testRetainedTransportIsStopped(bool prepared)
{
    TcpPortReserver reserver;
    reserver.registerScope(QStringLiteral("s5b"), new S5BServersProducer);
    auto *client = new Client;
    client->setTcpPortReserver(&reserver);
    auto *session   = client->jingleManager()->newSession(Jid(QStringLiteral("peer@example.test/device")));
    auto  transport = session->newOutgoingTransport(J::S5B::NS);
    check(bool(transport), "Cannot create externally retained transport");
    if (prepared)
        transport->prepare(); // Port/proxy discovery callbacks are queued but not dispatched.
    delete client;
    check(transport->state() == J::State::Finished, "Externally retained transport remained active after shutdown");
    for (const auto &channel : transport->channels())
        check(!channel->isOpen(), "Externally retained connection remained open after shutdown");
    transport.clear();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static void testManagerDestructionAndReentrancy()
{
    Client client;
    // A standalone manager has no Client::close() to finish its sessions first.
    auto                *manager = new J::Manager(&client);
    QPointer<J::Session> first(manager->newSession(Jid(QStringLiteral("first@example.test/device"))));
    QPointer<J::Session> second(manager->newSession(Jid(QStringLiteral("second@example.test/device"))));
    int                  terminated = 0;
    QObject::connect(first, &J::Session::terminated, QCoreApplication::instance(), [&] {
        ++terminated;
        delete second.data();
        check(!manager->newSession(Jid(QStringLiteral("new@example.test/device"))),
              "Shutting-down manager created another session");
    });
    delete manager;
    check(!first && !second && terminated == 1, "Reentrant sibling deletion left a live session");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Deliberately violate provider teardown without changing the production
// managers: detach their back-reference but leave the registration behind.
static QStringList deinitializationErrors;
static void        captureDeinitializationErrors(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtCriticalMsg && message.startsWith(QStringLiteral("Jingle: incorrect deinitialization:")))
        deinitializationErrors.append(message);
    else
        std::fprintf(stderr, "%s\n", qPrintable(message));
}

static void testProviderDestructionDiagnostics()
{
    Client     client;
    J::Manager manager(&client);
    const auto previousHandler = qInstallMessageHandler(captureDeinitializationErrors);
    deinitializationErrors.clear();

    auto *application = new FT::Manager;
    manager.registerApplication(application);
    delete application; // The normal destructor unregisters.
    auto *transport = new J::S5B::Manager;
    manager.registerTransport(transport);
    delete transport;
    check(deinitializationErrors.isEmpty(), "Normal provider teardown reported a deinitialization error");

    application = new FT::Manager;
    manager.registerApplication(application);
    application->setJingleManager(nullptr); // Incorrect: no unregisterApp().
    delete application;
    check(!manager.isRegisteredApplication(FT::NS), "Destroyed application left a stale registration");

    transport = new J::S5B::Manager;
    manager.registerTransport(transport);
    transport->setJingleManager(nullptr); // Incorrect: no unregisterTransport().
    delete transport;
    check(!manager.isRegisteredTransport(J::S5B::NS), "Destroyed transport left a stale registration");

    qInstallMessageHandler(previousHandler);
    check(deinitializationErrors.size() == 2, "Incorrect teardown did not report exactly one error per namespace");
    check(deinitializationErrors[0].contains(QStringLiteral("application"))
              && deinitializationErrors[0].contains(FT::NS),
          "Application teardown diagnostic omitted the provider kind or namespace");
    check(deinitializationErrors[1].contains(QStringLiteral("transport"))
              && deinitializationErrors[1].contains(J::S5B::NS),
          "Transport teardown diagnostic omitted the provider kind or namespace");
    manager.discoFeatures(); // No cleared QPointer may remain in either registry.
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    for (auto tail : { Tail::Unregistered, Tail::Active, Tail::Deferred, Tail::Unattached })
        testClientDestruction(tail);
    testRetainedTransportIsStopped(false);
    testRetainedTransportIsStopped(true);
    testManagerDestructionAndReentrancy();
    testProviderDestructionDiagnostics();
    qInfo("Jingle shutdown ownership/order regressions passed");
}
