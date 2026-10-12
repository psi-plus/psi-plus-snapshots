// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <iris/jingle-application.h>
#include <iris/jingle-session.h>
#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/xmpp_client.h>
#include <utility>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
static void flushDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

class Channel final : public J::Connection {
public:
    J::TransportFeatures features() const override { return J::TransportFeature::Reliable; }
    void                 connectedNow()
    {
        open(QIODevice::ReadWrite);
        emit connected();
    }
    void fail() { emit error(1); }

protected:
    qint64 readDataInternal(char *, qint64) override { return 0; }
};

class Transport final : public J::Transport {
public:
    Transport(J::TransportManagerPad *pad, J::Origin creator) : J::Transport(pad, creator) { }
    QSharedPointer<Channel> channel = QSharedPointer<Channel>::create();
    std::function<void()>   onStop, onAdd;
    void                    forceState(J::State state) { setState(state); }
    bool deliver(J::Connection::Ptr connection) { return notifyIncomingConnection(std::move(connection)); }
    void stop() override
    {
        J::Transport::stop();
        const auto callback = onStop;
        if (callback)
            callback();
    }
    void                           prepare() override { }
    void                           start() override { }
    bool                           update(const QDomElement &) override { return true; }
    bool                           hasUpdates() const override { return false; }
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool = false) override { return {}; }
    bool                           isValid() const override { return true; }
    J::TransportFeatures           features() const override { return J::TransportFeature::Reliable; }
    J::Connection::Ptr             addChannel(J::TransportFeatures, const QString &, int = -1) override
    {
        const auto result   = channel;
        const auto callback = onAdd;
        if (callback)
            callback();
        return result;
    }
    QList<J::Connection::Ptr> channels() const override { return { channel }; }
};

class Selector final : public J::TransportSelector {
public:
    QPointer<J::Transport> next;
    int                    selections = 0;
    QPointer<J::Transport> getNextTransport() override
    {
        ++selections;
        return std::exchange(next, {});
    }
    QPointer<J::Transport> getAlikeTransport(QPointer<J::Transport>) override { return {}; }
    bool                   replace(QPointer<J::Transport>, QPointer<J::Transport>) override { return true; }
    void                   backupTransport(QPointer<J::Transport>) override { }
    bool                   hasMoreTransports() const override { return bool(next); }
    bool                   hasTransport(QPointer<J::Transport>) const override { return true; }
    int                    compare(QPointer<J::Transport>, QPointer<J::Transport>) const override { return 0; }
};

class Application final : public J::Application {
public:
    Application(J::ApplicationManagerPad *pad, QString name, J::Origin creator, J::Origin senders)
    {
        setParent(pad);
        _pad               = pad;
        _contentName       = std::move(name);
        _creator           = creator;
        _senders           = senders;
        _transportSelector = std::make_unique<Selector>();
    }
    Selector *selector() { return static_cast<Selector *>(_transportSelector.get()); }
    void      expect(std::function<void(J::Connection::Ptr)> callback)
    {
        expectSingleConnection(J::TransportFeature::Reliable, std::move(callback));
    }
    void advanceIqGeneration() { ++_transportReplaceGeneration; }
    void setState(J::State state) override
    {
        _state = state;
        emit stateChanged(state);
    }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    J::Reason                           lastReason() const override { return {}; }
    SetDescError                        setRemoteOffer(const QDomElement &) override { return Ok; }
    SetDescError                        setRemoteAnswer(const QDomElement &) override { return Ok; }
    QDomElement                         makeLocalOffer() override { return {}; }
    QDomElement                         makeLocalAnswer() override { return {}; }
    void                                prepare() override { }
    void                                start() override { }
    void remove(J::Reason::Condition, const QString &) override { setState(J::State::Finished); }
    void incomingRemove(const J::Reason &) override { setState(J::State::Finished); }

protected:
    void prepareTransport() override { }

private:
    std::optional<Stanza::Error> error_;
};

static const QString ns = QStringLiteral("urn:iris:test:connection-waiter");
class Manager;
class Pad final : public J::ApplicationManagerPad {
public:
    Pad(J::Session *session, J::ApplicationManager *manager) :
        J::ApplicationManagerPad(session), session_(session), manager_(manager)
    {
    }
    ~Pad() override { destroyOwnedObjects(); }
    J::Session            *session() const override { return session_; }
    J::ApplicationManager *manager() const override { return manager_; }
    QString                ns() const override { return ::ns; }
    QString                generateContentName(J::Origin) override { return QString::number(++sequence_); }

private:
    J::Session            *session_;
    J::ApplicationManager *manager_;
    int                    sequence_ = 0;
};
class Manager final : public J::ApplicationManager {
public:
    void            setJingleManager(J::Manager *) override { }
    J::Application *startApplication(J::ApplicationManagerPad *pad, const QString &name, J::Origin creator,
                                     J::Origin senders) override
    {
        return new Application(pad, name, creator, senders);
    }
    J::ApplicationManagerPad *pad(J::Session *session) override { return new Pad(session, this); }
    void                      closeAll(const QString & = {}) override { }
    QStringList               discoFeatures() const override { return { ::ns }; }
};
struct Fixture {
    Manager               manager;
    Client                client;
    QPointer<J::Session>  session;
    QPointer<Application> app;
    Fixture()
    {
        client.jingleManager()->registerApplication(&manager);
        session = client.jingleManager()->newSession(Jid(QStringLiteral("peer@example.test/device")));
        app     = static_cast<Application *>(session->newContent(ns));
        check(bool(app), "waiter fixture could not create application");
    }
    ~Fixture()
    {
        delete session.data();
        flushDeletes();
    }
    Transport *transport(J::Origin creator = J::Origin::Initiator)
    {
        return new Transport(session->transportPadFactory(J::IBB::NS), creator);
    }
    void install(Transport *transport)
    {
        check(app->setTransport(transport), "waiter fixture could not install transport");
        app->setState(J::State::Connecting);
    }
};

static void testReplacedTransport(bool retainOld, bool sameObject)
{
    Fixture             f;
    QPointer<Transport> old(f.transport());
    f.install(old);
    const auto            staleChannel = old->channel;
    QPointer<Application> sibling;
    if (retainOld) {
        sibling = static_cast<Application *>(f.session->newContent(ns));
        check(sibling->setTransport(old), "shared transport fixture failed");
    }
    int staleReady = 0, ready = 0;
    f.app->expect([&](J::Connection::Ptr) { ++staleReady; });
    old->onStop = [staleChannel] { staleChannel->connectedNow(); };
    auto next   = sameObject ? old.data() : f.transport();
    if (sameObject)
        old->channel = QSharedPointer<Channel>::create();
    check(f.app->setTransport(next), "replacement fixture failed");
    staleChannel->connectedNow();
    staleChannel->fail();
    check(staleReady == 0 && f.app->transport() == next, "retired transport published a channel or changed selection");
    f.app->expect([&](J::Connection::Ptr connection) {
        check(connection == next->channel, "wrong current channel");
        ++ready;
    });
    // Signaling acknowledgements must not invalidate channel negotiation.
    f.app->advanceIqGeneration();
    next->channel->connectedNow();
    check(ready == 1, "current channel did not become ready after replacement/IQ acknowledgement");
    if (retainOld)
        check(bool(old), "sibling application lost its live transport");
}

static void testLateRemoteChannel(bool fail, bool finishing)
{
    Fixture f;
    auto    old = f.transport(J::Origin::Responder);
    f.install(old);
    auto next       = f.transport();
    int  staleReady = 0, ready = 0;
    f.app->expect([&](J::Connection::Ptr) { ++staleReady; });
    if (finishing)
        f.app->setState(J::State::Finishing);
    else if (fail) {
        f.app->selector()->next = next;
        old->forceState(J::State::Finished);
        check(f.app->transport() == next && f.app->selector()->selections == 1, "failure did not select one fallback");
    } else
        check(f.app->setTransport(next), "remote waiter replacement failed");
    auto channel = QSharedPointer<Channel>::create();
    channel->connectedNow();
    check(!old->deliver(channel) && staleReady == 0, "retired acceptor consumed a late incoming channel");
    if (!finishing) {
        f.app->expect([&](J::Connection::Ptr) { ++ready; });
        next->channel->connectedNow();
        check(ready == 1, "late remote channel disturbed fallback channel");
    }
}

static void testDestroyedTransport()
{
    Fixture f;
    auto    old = f.transport();
    f.install(old);
    auto staleChannel       = old->channel;
    auto next               = f.transport();
    f.app->selector()->next = next;
    int ready               = 0;
    f.app->expect([&](J::Connection::Ptr) { ++ready; });
    delete old;
    staleChannel->connectedNow();
    check(ready == 0 && f.app->transport() == next && f.app->selector()->selections == 1,
          "destroyed transport retained its waiter or failed to select fallback");
}

static void testReadyCallbackDeletesOwner(bool deleteSession, bool remote)
{
    Fixture f;
    auto    tr = f.transport(remote ? J::Origin::Responder : J::Origin::Initiator);
    f.install(tr);
    const auto channel = tr->channel;
    int        ready   = 0;
    f.app->expect([&](J::Connection::Ptr) {
        ++ready;
        if (deleteSession)
            delete f.session.data();
        else
            delete f.app.data();
    });
    if (remote) {
        channel->connectedNow();
        check(tr->deliver(channel), "open remote channel was not accepted");
    } else
        channel->connectedNow();
    check(ready == 1 && !f.app, "ready callback did not delete owner safely");
}

static void testFailureCallbackReenters(bool sameObject)
{
    Fixture f;
    auto    tr = f.transport();
    f.install(tr);
    auto channel = tr->channel;
    int  ready   = 0;
    f.app->expect([&](J::Connection::Ptr) { ++ready; });
    if (sameObject) {
        f.app->selector()->next = f.transport();
        tr->onStop              = [&] {
            tr->onStop = {};
            tr->forceState(J::State::Connecting);
            check(f.app->setTransport(tr), "same-object reentrant selection failed");
        };
    } else
        tr->onStop = [&] { delete f.app.data(); };
    channel->fail();
    check(ready == 0, "failed channel was published ready");
    if (sameObject)
        check(f.app->transport() == tr && f.app->selector()->selections == 0,
              "old failure displaced reentrant transport selection");
    else
        check(!f.app, "failure callback did not delete application safely");
}

static void testChannelCreationReenters(bool destroyOwner, bool restartAttempt)
{
    Fixture f;
    auto    tr = f.transport();
    f.install(tr);
    auto staleChannel = tr->channel;
    auto next         = restartAttempt ? tr : f.transport();
    int  staleReady = 0, ready = 0;
    tr->onAdd = [&] {
        tr->onAdd = {};
        if (destroyOwner) {
            delete f.app.data();
            return;
        }
        if (restartAttempt)
            tr->channel = QSharedPointer<Channel>::create();
        else
            check(f.app->setTransport(next), "channel creation replacement failed");
        f.app->expect([&](J::Connection::Ptr) { ++ready; });
    };
    f.app->expect([&](J::Connection::Ptr) { ++staleReady; });
    staleChannel->connectedNow();
    check(staleReady == 0, "channel creation resurrected retired attempt");
    if (!destroyOwner) {
        next->channel->connectedNow();
        check(ready == 1, "nested channel attempt was lost");
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testReplacedTransport(false, false);
    testReplacedTransport(true, false);
    testReplacedTransport(false, true);
    testLateRemoteChannel(false, false);
    testLateRemoteChannel(true, false);
    testLateRemoteChannel(false, true);
    testDestroyedTransport();
    for (bool deleteSession : { false, true })
        for (bool remote : { false, true })
            testReadyCallbackDeletesOwner(deleteSession, remote);
    testFailureCallbackReenters(false);
    testFailureCallbackReenters(true);
    testChannelCreationReenters(true, false);
    testChannelCreationReenters(false, false);
    testChannelCreationReenters(false, true);
    qInfo("Connection waiter lifetime regressions passed");
    return 0;
}
