// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QDomDocument>
#include <QPointer>
#include <QtCrypto>

#include <iris/jingle-application.h>
#define private public
#include <iris/jingle-session.h>
#undef private
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>

#include <memory>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Result : public Task {
public:
    Result(Task *parent, bool ok) : Task(parent)
    {
        if (ok)
            setSuccess();
        else
            setError(500);
    }
};

class TestTransportManager;

class TestTransportPad : public J::TransportManagerPad {
public:
    TestTransportPad(J::Session *session, TestTransportManager *manager) : session_(session), manager_(manager) { }
    J::Session          *session() const override { return session_; }
    QString              ns() const override;
    J::TransportManager *manager() const override;

private:
    J::Session           *session_;
    TestTransportManager *manager_;
};

class TestTransport : public J::Transport {
public:
    TestTransport(const J::TransportManagerPad::Ptr &pad, J::Origin creator, QString id = {}) :
        J::Transport(pad, creator), id_(std::move(id))
    {
    }

    void           forceState(J::State state) { setState(state); }
    void           setHasUpdates(bool value) { hasUpdates_ = value; }
    int            starts() const { return starts_; }
    const QString &id() const { return id_; }

    void prepare() override
    {
        if (state() < J::State::ApprovedToSend)
            setState(J::State::ApprovedToSend);
        hasUpdates_ = true;
        emit updated();
    }
    void start() override
    {
        ++starts_;
        setState(J::State::Active);
    }
    class Prepared : public J::Transport::PreparedUpdate {
    public:
        explicit Prepared(QString id) : id(std::move(id)) { }
        QString id;
    };

    PrepareUpdateResult prepareUpdate(const QDomElement &el) override
    {
        if (el.attribute(QStringLiteral("parse")) == QLatin1String("fail"))
            return { PrepareUpdateStatus::Invalid, {}, {} };
        return { PrepareUpdateStatus::Ready,
                 std::make_unique<Prepared>(el.attribute(QStringLiteral("id"))), {} };
    }

    bool commitPreparedUpdate(PreparedUpdatePtr update) override
    {
        auto prepared = dynamic_cast<Prepared *>(update.get());
        if (!prepared)
            return false;
        id_ = prepared->id;
        if (state() == J::State::Created)
            setState(J::State::Pending);
        return true;
    }

    bool update(const QDomElement &el) override
    {
        auto prepared = prepareUpdate(el);
        return prepared && commitPreparedUpdate(std::move(prepared.update));
    }
    bool                           hasUpdates() const override { return hasUpdates_; }
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement) override
    {
        if (!hasUpdates_ && !ensureTransportElement)
            return {};
        auto el = pad()->doc()->createElementNS(pad()->ns(), QStringLiteral("transport"));
        el.setAttribute(QStringLiteral("id"), id_);
        hasUpdates_ = false;
        setState(J::State::Unacked);
        return { el, [self = QPointer<TestTransport>(this)](Task *task) {
                    if (self && task && task->success())
                        self->setState(J::State::Pending);
                } };
    }
    bool                      isValid() const override { return true; }
    J::TransportFeatures      features() const override { return J::TransportFeature::Reliable; }
    J::Connection::Ptr        addChannel(J::TransportFeatures, const QString &, int) override { return {}; }
    QList<J::Connection::Ptr> channels() const override { return {}; }

private:
    QString id_;
    bool    hasUpdates_ = false;
    int     starts_     = 0;
};

class TestTransportManager : public J::TransportManager {
public:
    static QString               namespaceUri() { return QStringLiteral("urn:iris:test:transport-replace"); }
    J::TransportFeatures         features() const override { return J::TransportFeature::Reliable; }
    void                         setJingleManager(J::Manager *manager) override { manager_ = manager; }
    QSharedPointer<J::Transport> newTransport(const J::TransportManagerPad::Ptr &pad, J::Origin creator) override
    {
        return QSharedPointer<TestTransport>::create(pad, creator);
    }
    J::TransportManagerPad *pad(J::Session *session) override { return new TestTransportPad(session, this); }
    void                    closeAll(const QString & = QString()) override { }
    QStringList             ns() const override { return { namespaceUri() }; }
    QStringList             discoFeatures() const override { return { namespaceUri() }; }

private:
    J::Manager *manager_ = nullptr;
};

QString              TestTransportPad::ns() const { return TestTransportManager::namespaceUri(); }
J::TransportManager *TestTransportPad::manager() const { return manager_; }

class TestAppPad : public J::ApplicationManagerPad {
public:
    explicit TestAppPad(J::Session *session) : session_(session) { }
    J::Session            *session() const override { return session_; }
    QString                ns() const override { return QStringLiteral("urn:iris:test:application"); }
    J::ApplicationManager *manager() const override { return nullptr; }
    QString                generateContentName(J::Origin) override { return QStringLiteral("test"); }

private:
    J::Session *session_;
};

class TestSelector : public J::TransportSelector {
public:
    QSharedPointer<J::Transport> getNextTransport() override
    {
        ++getNextCalls;
        if (next_.isEmpty())
            return {};
        return next_.takeFirst();
    }
    QSharedPointer<J::Transport> getAlikeTransport(QSharedPointer<J::Transport>) override
    {
        ++getAlikeCalls;
        auto result = alike_;
        alike_.clear();
        return result;
    }
    bool replace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) override
    {
        ++replaceCalls;
        return allowReplace;
    }
    void backupTransport(QSharedPointer<J::Transport> transport) override
    {
        ++backupCalls;
        next_.append(std::move(transport));
    }
    bool hasMoreTransports() const override { return bool(alike_) || !next_.isEmpty(); }
    bool hasTransport(QSharedPointer<J::Transport>) const override { return allowCanReplace; }
    bool canReplace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) override
    {
        ++canReplaceCalls;
        return allowCanReplace;
    }
    int  compare(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) const override { return 1; }
    void setAlike(const QSharedPointer<J::Transport> &transport) { alike_ = transport; }

    bool allowCanReplace = true;
    bool allowReplace    = true;
    int  canReplaceCalls = 0;
    int  replaceCalls    = 0;
    int  getAlikeCalls   = 0;
    int  getNextCalls    = 0;
    int  backupCalls     = 0;

private:
    QSharedPointer<J::Transport>        alike_;
    QList<QSharedPointer<J::Transport>> next_;
};

class TestApplication : public J::Application {
public:
    TestApplication(J::Session *session, QString name, J::Origin creator)
    {
        _pad.reset(new TestAppPad(session));
        _contentName = std::move(name);
        _creator     = creator;
        _senders     = J::Origin::Both;
    }

    TestSelector *installTransport(const QSharedPointer<TestTransport> &transport,
                                   std::unique_ptr<TestSelector>        selector)
    {
        auto raw           = selector.get();
        _transport         = transport;
        _transportSelector = std::move(selector);
        return raw;
    }
    void setReplaceEnabled(bool enabled) { replaceEnabled_ = enabled; }
    void markReplacePlanned() { _pendingTransportReplace = PendingTransportReplace::Planned; }
    void markReplaceAwaitingAck()
    {
        // Handler tests still need a real coordinator transaction. NeedAck alone
        // is not collision lifetime (in particular inside batch ACK callbacks).
        _pendingTransportReplace = PendingTransportReplace::Planned;
        _update                  = { J::Action::TransportReplace, {} };
        static_cast<TestTransport *>(_transport.data())->setHasUpdates(true);
        const auto update = takeOutgoingUpdate();
        auto       xml    = _pad->doc()->createElementNS(J::NS, QStringLiteral("jingle"));
        for (const auto &element : std::get<0>(update))
            xml.appendChild(element);
        _pad->tieBreaker()->outgoingStarted(J::Action::TransportReplace, xml);
    }
    void markReplaceInProgress() { _pendingTransportReplace = PendingTransportReplace::InProgress; }
    bool replacePlanned() const { return _pendingTransportReplace == PendingTransportReplace::Planned; }
    bool replaceNeedAck() const { return _pendingTransportReplace == PendingTransportReplace::NeedAck; }
    bool replaceInProgress() const { return _pendingTransportReplace == PendingTransportReplace::InProgress; }
    bool replaceIdle() const { return _pendingTransportReplace == PendingTransportReplace::None; }

    void setState(J::State state) override
    {
        if (_state == state)
            return;
        _state = state;
        emit stateChanged(state);
    }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    J::Reason                           lastReason() const override { return {}; }
    SetDescError                        setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError                        setRemoteAnswer(const QDomElement &) override { return Unparsed; }
    QDomElement                         makeLocalOffer() override { return {}; }
    QDomElement                         makeLocalAnswer() override { return {}; }
    bool                                isTransportReplaceEnabled() const override { return replaceEnabled_; }
    void                                prepare() override { }
    void                                start() override { }
    void                                remove(J::Reason::Condition, const QString &) override { }
    void                                incomingRemove(const J::Reason &) override { }

protected:
    void prepareTransport() override
    {
        if (_transport)
            _transport->prepare();
    }

private:
    bool                         replaceEnabled_ = true;
    std::optional<Stanza::Error> error_;
};

static QSharedPointer<TestTransport> makeTransport(J::Session &session, J::Origin creator, J::State state,
                                                   const QString &id)
{
    auto pad = session.transportPadFactory(TestTransportManager::namespaceUri());
    check(bool(pad), "test transport pad missing");
    auto transport = QSharedPointer<TestTransport>::create(pad, creator, id);
    transport->forceState(state);
    return transport;
}

static TestApplication *addApplication(J::Session &session, const QString &name, J::Origin contentCreator,
                                       const QSharedPointer<TestTransport> &transport,
                                       std::unique_ptr<TestSelector> selector, TestSelector **selectorOut = nullptr,
                                       J::State state = J::State::Active)
{
    auto app = new TestApplication(&session, name, contentCreator);
    auto raw = app->installTransport(transport, std::move(selector));
    app->setState(state);
    session.addContent(app);
    if (selectorOut)
        *selectorOut = raw;
    return app;
}

struct ReplaceSpec {
    QString   name;
    J::Origin creator;
    QString   transportNs;
    QString   id;
    bool      parse = true;
};

static QDomElement makeReplace(QDomDocument &doc, std::initializer_list<ReplaceSpec> specs)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    for (const auto &spec : specs) {
        auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
        J::ContentBase::setCreatorAttr(content, spec.creator);
        content.setAttribute(QStringLiteral("name"), spec.name);
        auto transport = doc.createElementNS(spec.transportNs, QStringLiteral("transport"));
        transport.setAttribute(QStringLiteral("id"), spec.id);
        if (!spec.parse)
            transport.setAttribute(QStringLiteral("parse"), QStringLiteral("fail"));
        content.appendChild(transport);
        jingle.appendChild(content);
    }
    return jingle;
}

static bool isTieBreak(const J::Session &session)
{
    const auto error = session.lastError();
    return error && J::ErrorUtil::jingleCondition(*error) == J::ErrorUtil::TieBreak;
}

static void testInitiatorLocalUnackedTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       old            = makeTransport(session, J::Origin::Initiator, J::State::Unacked, QStringLiteral("old"));
    auto       selector       = std::make_unique<TestSelector>();
    selector->allowCanReplace = false;
    TestSelector *selectorRaw = nullptr;
    auto          app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                                       &selectorRaw);
    app->markReplaceAwaitingAck();

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Initiator,
                                                TestTransportManager::namespaceUri(), QStringLiteral("incoming") } }));
    check(!ok && isTieBreak(session), "initiator did not tie-break crossed transport-replace");
    check(app->transport().data() == old.data(), "tie-break replaced the initiator transport");
    check(selectorRaw->canReplaceCalls == 0 && selectorRaw->replaceCalls == 0,
          "tie-break consulted or mutated transport selector");
}

static void testResponderAcceptsInitiatorWinner(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Responder);
    auto       old            = makeTransport(session, J::Origin::Responder, J::State::Unacked, QStringLiteral("old"));
    auto       selector       = std::make_unique<TestSelector>();
    TestSelector *selectorRaw = nullptr;
    auto          app = addApplication(session, QStringLiteral("audio"), J::Origin::Responder, old, std::move(selector),
                                       &selectorRaw);

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Responder,
                                                TestTransportManager::namespaceUri(), QStringLiteral("winner") } }));
    check(ok && !isTieBreak(session), "responder rejected initiator transport-replace winner");
    check(app->transport().data() != old.data() && app->transport()->creator() == J::Origin::Initiator,
          "responder did not install initiator transport");
    check(static_cast<TestTransport *>(app->transport().data())->id() == QLatin1String("winner"),
          "responder installed wrong replacement transport");
    check(selectorRaw->canReplaceCalls == 1 && selectorRaw->replaceCalls == 1,
          "responder replacement did not pass through selector");
    check(app->replaceInProgress(), "incoming replacement did not enter InProgress state");
}

static void testInitiatorPendingTransportAcceptsRemoteReplace(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       old = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
    auto       app
        = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::make_unique<TestSelector>());
    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReplace,
                                makeReplace(doc,
                                            { { QStringLiteral("audio"), J::Origin::Initiator,
                                                TestTransportManager::namespaceUri(), QStringLiteral("remote") } }));
    check(ok && !isTieBreak(session), "acknowledged local transport incorrectly caused tie-break");
    check(app->transport().data() != old.data() && app->replaceInProgress(),
          "remote replacement was not installed after local transport ACK");
}

static void testRemoteUnackedTransportDoesNotTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       old = makeTransport(session, J::Origin::Responder, J::State::Unacked, QStringLiteral("old-remote"));
    auto       app
        = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::make_unique<TestSelector>());
    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("new-remote") } }));
    check(ok && !isTieBreak(session), "remote Unacked transport incorrectly caused tie-break");
    check(app->transport().data() != old.data(), "remote replacement was not installed");
}

static void testBatchTieBreakReevaluatesSiblingTransport(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       audioOld = makeTransport(session, J::Origin::Initiator, J::State::Unacked, QStringLiteral("audio-old"));
    auto       audio    = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                         std::make_unique<TestSelector>());
    audio->markReplaceAwaitingAck();
    auto videoOld      = makeTransport(session, J::Origin::Responder, J::State::Active, QStringLiteral("video-old"));
    auto videoSelector = std::make_unique<TestSelector>();
    auto videoRetry    = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("video-local"));
    videoSelector->setAlike(videoRetry);
    TestSelector *videoSelectorRaw = nullptr;
    auto          video            = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                                    std::move(videoSelector), &videoSelectorRaw);

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-remote") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-remote") } }));
    check(!ok && isTieBreak(session), "batch collision did not return tie-break");
    check(audio->transport().data() == audioOld.data(), "colliding content changed its transport");
    check(video->transport().data() == videoRetry.data(),
          "tie-break did not reselect sibling transport against remote proposal");
    check(videoSelectorRaw->getAlikeCalls == 1 && videoSelectorRaw->replaceCalls == 1,
          "sibling transport was not reselected through getAlikeTransport");
    check(video->replacePlanned(), "sibling local retry was not marked as planned transport-replace");
}

static void testBatchTieBreakKeepsPreparedLocalSibling(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       audioOld = makeTransport(session, J::Origin::Initiator, J::State::Unacked, QStringLiteral("audio-old"));
    auto       audio    = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                         std::make_unique<TestSelector>());
    audio->markReplaceAwaitingAck();
    auto videoOld = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("video-old"));
    auto videoSelector = std::make_unique<TestSelector>();
    auto videoRetry    = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("video-retry"));
    videoSelector->setAlike(videoRetry);
    TestSelector *videoSelectorRaw = nullptr;
    auto          video            = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                                    std::move(videoSelector), &videoSelectorRaw);

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-remote") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("video-remote") } }));
    check(!ok && isTieBreak(session), "prepared-local batch collision did not tie-break");
    check(video->transport().data() == videoOld.data(), "tie-break replaced a not-yet-sent local sibling transport");
    check(videoSelectorRaw->getAlikeCalls == 0 && videoSelectorRaw->replaceCalls == 0,
          "prepared local sibling unexpectedly entered reselection");
}

static void testMalformedBatchAbortsBeforeTieBreakSideEffects(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       audioOld = makeTransport(session, J::Origin::Initiator, J::State::Unacked, QStringLiteral("audio-old"));
    auto       audio    = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                         std::make_unique<TestSelector>());
    audio->markReplaceAwaitingAck();
    auto videoOld = makeTransport(session, J::Origin::Responder, J::State::Active, QStringLiteral("video-old"));
    auto video    = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                   std::make_unique<TestSelector>());

    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-remote") },
                        { QStringLiteral("video"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("broken"), false } }));
    check(!ok && !isTieBreak(session), "malformed batch was incorrectly reported as tie-break");
    check(audio->transport().data() == audioOld.data() && video->transport().data() == videoOld.data(),
          "malformed batch mutated transports before validation completed");
}

static void testUnknownTransportIsRejectedWithoutMutation(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       old = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
    auto       app
        = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::make_unique<TestSelector>());
    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator,
                          QStringLiteral("urn:iris:test:unknown-transport"), QStringLiteral("unknown") } }));
    check(ok && app->transport().data() == old.data(), "unsupported transport mutated existing transport");
}

static void testDisabledAndSelectorRejectedReplaceRemainUnchanged(Client &client)
{
    {
        J::Session    session(client.jingleManager(), Jid(QStringLiteral("disabled@example.test/device")),
                              J::Origin::Initiator);
        auto          old      = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
        auto          selector = std::make_unique<TestSelector>();
        TestSelector *selectorRaw = nullptr;
        auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                                  &selectorRaw);
        app->setReplaceEnabled(false);
        QDomDocument doc;
        const bool   ok = session.updateFromXml(
            J::Action::TransportReplace,
            makeReplace(doc,
                          { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                              QStringLiteral("incoming") } }));
        check(ok && app->transport().data() == old.data(), "disabled transport-replace changed transport");
        check(selectorRaw->canReplaceCalls == 1 && selectorRaw->replaceCalls == 0,
              "disabled transport-replace unexpectedly called selector replace");
    }
    {
        J::Session session(client.jingleManager(), Jid(QStringLiteral("selector@example.test/device")),
                           J::Origin::Initiator);
        auto       old      = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("old"));
        auto       selector = std::make_unique<TestSelector>();
        selector->allowCanReplace = false;
        TestSelector *selectorRaw = nullptr;
        auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, old, std::move(selector),
                                  &selectorRaw);
        QDomDocument doc;
        const bool   ok = session.updateFromXml(
            J::Action::TransportReplace,
            makeReplace(doc,
                          { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                              QStringLiteral("incoming") } }));
        check(ok && app->transport().data() == old.data(), "selector-rejected transport changed current transport");
        check(selectorRaw->canReplaceCalls == 1 && selectorRaw->replaceCalls == 0,
              "selector rejection still called replace");
    }
}

static void testTieBreakDominatesUnsupportedSibling(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       audioOld = makeTransport(session, J::Origin::Initiator, J::State::Unacked, QStringLiteral("audio-old"));
    auto       audio    = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, audioOld,
                                         std::make_unique<TestSelector>());
    audio->markReplaceAwaitingAck();
    auto         videoOld = makeTransport(session, J::Origin::Responder, J::State::Active, QStringLiteral("video-old"));
    auto         video    = addApplication(session, QStringLiteral("video"), J::Origin::Initiator, videoOld,
                                           std::make_unique<TestSelector>());
    QDomDocument doc;
    const bool   ok = session.updateFromXml(
        J::Action::TransportReplace,
        makeReplace(doc,
                      { { QStringLiteral("audio"), J::Origin::Initiator, TestTransportManager::namespaceUri(),
                          QStringLiteral("audio-remote") },
                        { QStringLiteral("video"), J::Origin::Initiator,
                          QStringLiteral("urn:iris:test:unknown-transport"), QStringLiteral("unsupported") } }));
    check(!ok && isTieBreak(session), "tie-break did not dominate unsupported sibling in same IQ");
    check(audio->transport().data() == audioOld.data() && video->transport().data() == videoOld.data(),
          "mixed tie-break/unsupported batch mutated current transports");
}

static void testOutgoingReplaceLifecycle(Client &client, Task *success)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       replacement
        = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("replacement"));
    replacement->setHasUpdates(true);
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, replacement,
                              std::make_unique<TestSelector>(), nullptr, J::State::Connecting);
    app->markReplacePlanned();
    check(app->evaluateOutgoingUpdate().action == J::Action::TransportReplace,
          "planned replacement did not evaluate to transport-replace");
    auto update = app->takeOutgoingUpdate();
    check(app->replaceNeedAck(), "sending transport-replace did not enter NeedAck");
    const auto &ack = std::get<1>(update);
    check(bool(ack), "transport-replace had no ACK callback");
    ack(success);
    check(app->replaceInProgress(), "successful transport-replace ACK did not enter InProgress");
}

static void testRemoteTransportAcceptLifecycle(Client &client, Task *success)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto remote = makeTransport(session, J::Origin::Responder, J::State::ApprovedToSend, QStringLiteral("remote"));
    remote->setHasUpdates(true);
    auto app = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, remote,
                              std::make_unique<TestSelector>(), nullptr, J::State::Connecting);
    app->markReplaceInProgress();
    check(app->evaluateOutgoingUpdate().action == J::Action::TransportAccept,
          "remote replacement did not evaluate to transport-accept");
    auto        update = app->takeOutgoingUpdate();
    const auto &ack    = std::get<1>(update);
    check(bool(ack), "transport-accept had no ACK callback");
    ack(success);
    check(app->replaceIdle() && remote->starts() == 1 && remote->state() == J::State::Active,
          "successful transport-accept did not finish replacement and start transport");
}

static void testIncomingTransportAcceptIgnoresOutOfOrder(Client &client)
{
    J::Session   session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto         local = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local"));
    auto         app   = addApplication(session, QStringLiteral("audio"), J::Origin::Initiator, local,
                                        std::make_unique<TestSelector>(), nullptr, J::State::Connecting);
    QDomDocument doc;
    auto         transport = doc.createElementNS(TestTransportManager::namespaceUri(), QStringLiteral("transport"));
    transport.setAttribute(QStringLiteral("id"), QStringLiteral("accepted"));
    app->incomingTransportAccept(transport);
    check(local->starts() == 0 && app->replaceIdle(), "out-of-order transport-accept changed replacement state");
    app->markReplaceInProgress();
    app->incomingTransportAccept(transport);
    check(local->starts() == 1 && app->replaceIdle(), "valid transport-accept did not complete replacement");
}

int main(int argc, char **argv)
{
    QCoreApplication     app(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);
    Result success(client.rootTask(), true);

    testInitiatorLocalUnackedTieBreak(client);
    testResponderAcceptsInitiatorWinner(client);
    testInitiatorPendingTransportAcceptsRemoteReplace(client);
    testRemoteUnackedTransportDoesNotTieBreak(client);
    testBatchTieBreakReevaluatesSiblingTransport(client);
    testBatchTieBreakKeepsPreparedLocalSibling(client);
    testMalformedBatchAbortsBeforeTieBreakSideEffects(client);
    testUnknownTransportIsRejectedWithoutMutation(client);
    testDisabledAndSelectorRejectedReplaceRemainUnchanged(client);
    testTieBreakDominatesUnsupportedSibling(client);
    testOutgoingReplaceLifecycle(client, &success);
    testRemoteTransportAcceptLifecycle(client, &success);
    testIncomingTransportAcceptIgnoresOutOfOrder(client);

    qInfo("Transport-replace legacy behavior regressions passed");
    return 0;
}
