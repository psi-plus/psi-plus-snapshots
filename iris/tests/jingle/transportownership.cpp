// SPDX-License-Identifier: LGPL-2.1-or-later
#include <functional>
#include <iris/jingle-application.h>
#include <memory>
#define private public
#include <iris/jingle-session.h>
#undef private
#include <iris/jingle-ft.h>
#include <iris/tcpportreserver.h>
#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>

#include <QCoreApplication>
#include <QPointer>

using namespace XMPP;
namespace J  = XMPP::Jingle;
namespace FT = J::FileTransfer;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static void flushDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

class CallbackTransport final : public J::Transport {
public:
    explicit CallbackTransport(J::TransportManagerPad *pad) : Transport(pad, J::Origin::Initiator) { }
    std::function<void()> onStop;
    void                  stop() override
    {
        Transport::stop();
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
    J::Connection::Ptr             addChannel(J::TransportFeatures, const QString &, int = -1) override { return {}; }
    QList<J::Connection::Ptr>      channels() const override { return {}; }
};

class CallbackSelector final : public J::TransportSelector {
public:
    std::function<void()> onReplace;
    bool                  replace(QPointer<J::Transport>, QPointer<J::Transport>) override
    {
        const auto callback = onReplace;
        if (callback)
            callback();
        return true;
    }
    QPointer<J::Transport> getNextTransport() override { return {}; }
    QPointer<J::Transport> getAlikeTransport(QPointer<J::Transport>) override { return {}; }
    void                   backupTransport(QPointer<J::Transport>) override { }
    bool                   hasMoreTransports() const override { return false; }
    bool                   hasTransport(QPointer<J::Transport>) const override { return true; }
    int                    compare(QPointer<J::Transport>, QPointer<J::Transport>) const override { return 0; }
};

class CallbackApplication final : public J::Application {
public:
    CallbackApplication(J::ApplicationManagerPad *pad, QString name, J::Origin creator, J::Origin senders,
                        std::shared_ptr<int> destroyed) : destroyed_(std::move(destroyed))
    {
        setParent(pad);
        _pad               = pad;
        _contentName       = std::move(name);
        _creator           = creator;
        _senders           = senders;
        _transportSelector = std::make_unique<CallbackSelector>();
    }
    ~CallbackApplication() override { ++*destroyed_; }
    CallbackSelector     *selector() { return static_cast<CallbackSelector *>(_transportSelector.get()); }
    std::function<void()> onOffer;
    SetDescError          offerResult = Ok;
    SetDescError          setRemoteOffer(const QDomElement &) override
    {
        const auto callback = onOffer;
        const auto result   = offerResult;
        if (callback)
            callback();
        return result;
    }
    SetDescError                        setRemoteAnswer(const QDomElement &) override { return Ok; }
    QDomElement                         makeLocalOffer() override { return {}; }
    QDomElement                         makeLocalAnswer() override { return {}; }
    void                                setState(J::State state) override { _state = state; }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    J::Reason                           lastReason() const override { return {}; }
    void                                prepare() override { }
    void                                start() override { }
    void                                remove(J::Reason::Condition, const QString &) override { }
    void                                incomingRemove(const J::Reason &) override { }

protected:
    void prepareTransport() override { }

private:
    std::shared_ptr<int>         destroyed_;
    std::optional<Stanza::Error> error_;
};

static const QString callbackNs = QStringLiteral("urn:iris:test:ownership-callback");

class CallbackPad final : public J::ApplicationManagerPad {
public:
    CallbackPad(J::Session *session, J::ApplicationManager *manager) :
        ApplicationManagerPad(session), session_(session), manager_(manager)
    {
    }
    ~CallbackPad() override { destroyOwnedObjects(); }
    J::Session            *session() const override { return session_; }
    J::ApplicationManager *manager() const override { return manager_; }
    QString                ns() const override { return callbackNs; }
    QString                generateContentName(J::Origin) override { return QStringLiteral("callback"); }

private:
    J::Session            *session_;
    J::ApplicationManager *manager_;
};

class CallbackManager final : public J::ApplicationManager {
public:
    std::shared_ptr<int>                       destroyed = std::make_shared<int>(0);
    std::function<void(CallbackApplication *)> configure;
    void                                       setJingleManager(J::Manager *) override { }
    J::ApplicationManagerPad *pad(J::Session *session) override { return new CallbackPad(session, this); }
    J::Application           *startApplication(J::ApplicationManagerPad *pad, const QString &name, J::Origin creator,
                                               J::Origin senders) override
    {
        auto application = new CallbackApplication(pad, name, creator, senders, destroyed);
        if (configure)
            configure(application);
        return application;
    }
    void        closeAll(const QString & = QString()) override { }
    QStringList discoFeatures() const override { return { callbackNs }; }
};

static void testReentrantStop()
{
    for (int action = 0; action < 4; ++action) {
        CallbackManager manager;
        Client          client;
        client.jingleManager()->registerApplication(&manager);
        QPointer<J::Session>        session(client.jingleManager()->newSession(Jid("peer@example.test/device")));
        auto                        pad = session->transportPadFactory(J::IBB::NS);
        QPointer<CallbackTransport> old(new CallbackTransport(pad));
        QPointer<CallbackTransport> replacement(new CallbackTransport(pad));
        QPointer<CallbackTransport> successor(new CallbackTransport(pad));
        QPointer<J::Application>    application(session->newContent(callbackNs));
        check(application && application->setTransport(old), "stop callback fixture was not installed");
        old->onStop = [&] {
            if (action == 0)
                delete application.data();
            else if (action == 1)
                delete session.data();
            else
                check(application->setTransport(action == 2 ? successor.data() : replacement.data()),
                      "nested transport replacement failed");
        };
        check(!application->setTransport(replacement), "retired replacement reported success after stop callback");
        if (action == 0)
            check(!application && session && replacement, "application deletion did not preserve the session owner");
        else if (action == 1)
            check(!session && !application && !replacement, "session deletion left transport handles alive");
        else {
            auto expected = action == 2 ? successor : replacement;
            check(application && application->transport() == expected, "outer replacement displaced nested selection");
            int updates = 0;
            QObject::connect(application, &J::Application::updated, application, [&] { ++updates; });
            emit expected->updated();
            check(updates == 1, "outer replacement duplicated nested transport callbacks");
        }
        if (session)
            delete session.data();
        flushDeletes();
    }
}

static QDomElement incomingContents(QDomDocument &doc)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    for (const auto &name : { QStringLiteral("first"), QStringLiteral("second") }) {
        auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
        content.setAttribute(QStringLiteral("name"), name);
        content.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
        content.setAttribute(QStringLiteral("senders"), QStringLiteral("both"));
        content.appendChild(doc.createElementNS(callbackNs, QStringLiteral("description")));
        auto transport = doc.createElementNS(J::IBB::NS, QStringLiteral("transport"));
        transport.setAttribute(QStringLiteral("sid"), name);
        transport.setAttribute(QStringLiteral("block-size"), QStringLiteral("4096"));
        content.appendChild(transport);
        jingle.appendChild(content);
    }
    return jingle;
}

static void testIncomingDeletion()
{
    for (bool initial : { false, true }) {
        for (bool duringOffer : { false, true }) {
            for (bool deleteSession : { false, true }) {
                CallbackManager manager;
                Client          client;
                client.jingleManager()->registerApplication(&manager);
                QPointer<J::Session> session(
                    new J::Session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Responder));
                manager.configure = [&](CallbackApplication *application) {
                    if (application->contentName() != QStringLiteral("second"))
                        return;
                    auto destroy = [&, application] {
                        if (deleteSession)
                            delete session.data();
                        else
                            delete application;
                    };
                    if (duringOffer) {
                        application->offerResult = J::Application::IncompatibleParameters;
                        application->onOffer     = destroy;
                    } else
                        application->selector()->onReplace = destroy;
                };
                QDomDocument doc;
                const auto   contents = incomingContents(doc);
                if (initial)
                    check(!session->incomingInitiate(J::Jingle(J::Action::SessionInitiate, "callback-test"), contents),
                          "cancelled incoming initiation reported success");
                else {
                    const bool result = session->updateFromXml(J::Action::ContentAdd, contents);
                    check(result == deleteSession, "cancelled content-add returned the wrong disposition");
                }
                check(*manager.destroyed == 2, "incoming parser retained or destroyed a candidate twice");
                check(bool(session) != deleteSession, "incoming callback destroyed the wrong owner");
                if (session)
                    delete session.data();
                flushDeletes();
            }
        }
    }
}

static void testIncomingManagerDeletion()
{
    for (bool duringOffer : { false, true }) {
        CallbackManager manager;
        Client          client;
        client.jingleManager()->registerApplication(&manager);
        manager.configure = [&](CallbackApplication *application) {
            if (application->contentName() != QStringLiteral("second"))
                return;
            auto destroy = [session = QPointer<J::Session>(application->pad()->session())] { delete session.data(); };
            if (duringOffer)
                application->onOffer = destroy;
            else
                application->selector()->onReplace = destroy;
        };
        QDomDocument doc;
        auto         contents = incomingContents(doc);
        contents.setAttribute(QStringLiteral("action"), QStringLiteral("session-initiate"));
        contents.setAttribute(QStringLiteral("sid"), QStringLiteral("callback-test"));
        auto iq = doc.createElement(QStringLiteral("iq"));
        iq.setAttribute(QStringLiteral("type"), QStringLiteral("set"));
        iq.setAttribute(QStringLiteral("from"), QStringLiteral("peer@example.test/device"));
        iq.setAttribute(QStringLiteral("id"), QStringLiteral("callback-test"));
        iq.appendChild(contents);
        check(client.rootTask()->take(iq), "dispatcher did not handle cancelled incoming initiation");
        check(!client.jingleManager()->session(Jid("peer@example.test/device"), QStringLiteral("callback-test")),
              "manager registered a destroyed incoming session");
        check(client.jingleManager()->lastError().has_value(), "manager lost the cancelled initiation error");
        check(*manager.destroyed == 2, "manager teardown retained or destroyed a candidate twice");
        flushDeletes();
    }
}

// Session owns pads, pads own applications/transports. Applications only
// reference transports, so a transport lives while some application uses it.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TcpPortReserver  reserver;
    Client           client;
    client.setTcpPortReserver(&reserver);

    auto                *session = client.jingleManager()->newSession(Jid(QStringLiteral("peer@example.test/device")));
    QPointer<J::Session> sessionGuard(session);

    QPointer<J::Application> first(session->newContent(FT::NS, J::Origin::Initiator));
    QPointer<J::Application> second(session->newContent(FT::NS, J::Origin::Initiator));
    check(first && second, "file transfer contents were not created");
    QPointer<J::ApplicationManagerPad> appPad(first->pad());
    check(appPad && appPad->parent() == session && first->parent() == appPad && second->parent() == appPad,
          "applications are not owned through their session pad");

    QPointer<J::Transport> initial(session->newOutgoingTransport(J::IBB::NS));
    QPointer<J::Transport> shared(session->newOutgoingTransport(J::IBB::NS));
    check(initial && shared, "IBB transports were not created");
    QPointer<J::TransportManagerPad> transportPad(initial->pad());
    check(transportPad && transportPad->parent() == session && initial->parent() == transportPad,
          "transport is not owned through its session pad");

    // Replacing a transport releases the one which no longer has users.
    check(first->setTransport(initial), "initial transport was not installed");
    check(first->setTransport(shared), "replacement transport was not installed");
    flushDeletes();
    check(!initial, "replaced transport outlived its last application");
    check(shared && first->transport() == shared, "replacement transport was not retained");

    // A transport shared by several contents (BUNDLE) lives until its last user goes away.
    check(second->setTransport(shared), "shared transport was not installed for the second content");
    delete first.data();
    flushDeletes();
    check(shared && second->transport() == shared, "shared transport was released while still in use");
    delete second.data();
    flushDeletes();
    check(!shared, "shared transport outlived its last application");
    check(sessionGuard && appPad && transportPad, "pads did not survive removal of individual contents");

    // A candidate which was never installed is released explicitly, while a
    // used transport ignores the request.
    QPointer<J::Transport>   candidate(session->newOutgoingTransport(J::IBB::NS));
    QPointer<J::Transport>   used(session->newOutgoingTransport(J::IBB::NS));
    QPointer<J::Application> third(session->newContent(FT::NS, J::Origin::Initiator));
    check(candidate && used && third && third->setTransport(used), "candidate fixture was not created");
    candidate->releaseIfUnused();
    used->releaseIfUnused();
    flushDeletes();
    check(!candidate, "unused candidate transport was not released");
    check(used && third->transport() == used, "transport in use was released");

    // Session destruction tears down the whole tree; external handles do not retain it.
    delete session;
    check(!sessionGuard && !appPad && !transportPad && !third && !used,
          "pads, applications or transports outlived their session");
    flushDeletes();
    testReentrantStop();
    testIncomingDeletion();
    testIncomingManagerDeletion();
    qInfo("Jingle transport ownership regressions passed");
    return 0;
}
