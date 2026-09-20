// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QDomDocument>
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
    const QString &id() const { return id_; }
    int            starts() const { return starts_; }

    void prepare() override
    {
        setState(J::State::ApprovedToSend);
        hasUpdates_ = true;
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
        return { el, [](Task *) { } };
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
    static QString namespaceUri() { return QStringLiteral("urn:iris:test:transport-replace-protocol"); }

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
    QString                generateContentName(J::Origin) override { return QStringLiteral("audio"); }

private:
    J::Session *session_;
};

class TestSelector : public J::TransportSelector {
public:
    QSharedPointer<J::Transport> getNextTransport() override
    {
        ++getNextCalls;
        auto next = next_;
        next_.clear();
        return next;
    }
    QSharedPointer<J::Transport> getAlikeTransport(QSharedPointer<J::Transport>) override { return {}; }
    bool                         replace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) override
    {
        ++replaceCalls;
        return true;
    }
    void backupTransport(QSharedPointer<J::Transport>) override { }
    bool hasMoreTransports() const override { return bool(next_); }
    bool hasTransport(QSharedPointer<J::Transport>) const override { return true; }
    bool canReplace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) override { return true; }
    int  compare(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) const override { return 1; }

    void setNext(const QSharedPointer<J::Transport> &next) { next_ = next; }

    int getNextCalls = 0;
    int replaceCalls = 0;

private:
    QSharedPointer<J::Transport> next_;
};

class TestApplication : public J::Application {
public:
    explicit TestApplication(J::Session *session)
    {
        _pad.reset(new TestAppPad(session));
        _contentName = QStringLiteral("audio");
        _creator     = J::Origin::Initiator;
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
    void markReplacePlanned() { _pendingTransportReplace = PendingTransportReplace::Planned; }
    void markReplaceInProgress() { _pendingTransportReplace = PendingTransportReplace::InProgress; }
    bool replaceNeedAck() const { return _pendingTransportReplace == PendingTransportReplace::NeedAck; }
    bool replaceInProgress() const { return _pendingTransportReplace == PendingTransportReplace::InProgress; }
    bool replacePlanned() const { return _pendingTransportReplace == PendingTransportReplace::Planned; }

    void                                setState(J::State state) override { _state = state; }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    J::Reason                           lastReason() const override { return {}; }
    SetDescError                        setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError                        setRemoteAnswer(const QDomElement &) override { return Unparsed; }
    QDomElement                         makeLocalOffer() override { return {}; }
    QDomElement                         makeLocalAnswer() override { return {}; }
    void                                prepare() override { }
    void                                start() override { }
    void                                remove(J::Reason::Condition, const QString &) override { }
    void                                incomingRemove(const J::Reason &) override { }

protected:
    void prepareTransport() override { }

private:
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

static TestApplication *addApplication(J::Session &session, const QSharedPointer<TestTransport> &transport,
                                       std::unique_ptr<TestSelector> selector, TestSelector **selectorOut = nullptr)
{
    auto app = new TestApplication(&session);
    auto raw = app->installTransport(transport, std::move(selector));
    app->setState(J::State::Connecting);
    session.addContent(app);
    if (selectorOut)
        *selectorOut = raw;
    return app;
}

static QDomElement payload(QDomDocument &doc, const QString &transportId = {}, bool malformed = false)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
    J::ContentBase::setCreatorAttr(content, J::Origin::Initiator);
    content.setAttribute(QStringLiteral("name"), QStringLiteral("audio"));
    auto transport = doc.createElementNS(TestTransportManager::namespaceUri(), QStringLiteral("transport"));
    if (!transportId.isEmpty())
        transport.setAttribute(QStringLiteral("id"), transportId);
    if (malformed)
        transport.setAttribute(QStringLiteral("parse"), QStringLiteral("fail"));
    content.appendChild(transport);
    jingle.appendChild(content);
    return jingle;
}

static QDomElement duplicatePayload(QDomDocument &doc, const QString &transportId)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    for (int i = 0; i < 2; ++i) {
        auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
        J::ContentBase::setCreatorAttr(content, J::Origin::Initiator);
        content.setAttribute(QStringLiteral("name"), QStringLiteral("audio"));
        auto transport = doc.createElementNS(TestTransportManager::namespaceUri(), QStringLiteral("transport"));
        transport.setAttribute(QStringLiteral("id"), transportId);
        content.appendChild(transport);
        jingle.appendChild(content);
    }
    return jingle;
}

static void testTransportRejectSelectsFallback(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       rejected = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("rejected"));
    auto       fallback = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("fallback"));
    auto       selector = std::make_unique<TestSelector>();
    selector->setNext(fallback);
    TestSelector *selectorRaw = nullptr;
    auto          app         = addApplication(session, rejected, std::move(selector), &selectorRaw);
    app->markReplaceInProgress();

    QDomDocument doc;
    const bool   ok = session.updateFromXml(J::Action::TransportReject, payload(doc, QStringLiteral("rejected")));

    check(ok, "valid transport-reject was not handled");
    check(app->transport().data() == fallback.data(), "transport-reject did not select the next local transport");
    check(app->replacePlanned(), "fallback after transport-reject was not planned as a new replacement");
    check(selectorRaw->getNextCalls == 1 && selectorRaw->replaceCalls == 1,
          "transport-reject did not run through transport selector recovery");
}

static void testFailedIqSelectsFallback(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto rejected = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("rejected"));
    rejected->setHasUpdates(true);
    auto fallback = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("fallback"));
    auto selector = std::make_unique<TestSelector>();
    selector->setNext(fallback);
    TestSelector *selectorRaw = nullptr;
    auto          app         = addApplication(session, rejected, std::move(selector), &selectorRaw);
    app->markReplacePlanned();

    check(app->evaluateOutgoingUpdate().action == J::Action::TransportReplace,
          "planned replacement did not evaluate to transport-replace");
    auto update = app->takeOutgoingUpdate();
    check(app->replaceNeedAck(), "outgoing transport-replace did not enter NeedAck");
    const auto &ack = std::get<1>(update);
    check(bool(ack), "outgoing transport-replace had no ACK callback");
    Result failure(client.rootTask(), false);
    ack(&failure);

    check(app->transport().data() == fallback.data(), "failed transport-replace IQ did not select fallback transport");
    check(app->replacePlanned(), "fallback after failed transport-replace IQ was not planned for signaling");
    check(selectorRaw->getNextCalls == 1 && selectorRaw->replaceCalls == 1,
          "failed transport-replace IQ did not run through transport selector recovery");
}

static void testMalformedTransportAcceptRejectedAtomically(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       local = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local"));
    auto       app   = addApplication(session, local, std::make_unique<TestSelector>());
    app->markReplaceInProgress();

    QDomDocument doc;
    const bool ok = session.updateFromXml(J::Action::TransportAccept, payload(doc, QStringLiteral("malformed"), true));

    check(!ok, "malformed transport-accept was acknowledged by Session");
    check(app->replaceInProgress(), "malformed transport-accept committed replacement state");
    check(app->transport().data() == local.data() && local->starts() == 0,
          "malformed transport-accept changed or started the current transport");
}

static void testDuplicateTransportAcceptRejectedBeforeMutation(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("duplicate-accept@example.test/device")),
                       J::Origin::Initiator);
    auto       local = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local"));
    auto       app   = addApplication(session, local, std::make_unique<TestSelector>());
    app->markReplaceInProgress();

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportAccept, duplicatePayload(doc, QStringLiteral("duplicate")));

    check(!ok, "duplicate transport-accept contents were accepted");
    check(app->transport().data() == local.data() && app->replaceInProgress() && local->starts() == 0,
          "duplicate transport-accept mutated replacement state before rejection");
}

static void testDuplicateTransportRejectRejectedBeforeRecovery(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("duplicate-reject@example.test/device")),
                       J::Origin::Initiator);
    auto       local    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local"));
    auto       fallback = makeTransport(session, J::Origin::Initiator, J::State::Created, QStringLiteral("fallback"));
    auto       selector = std::make_unique<TestSelector>();
    selector->setNext(fallback);
    TestSelector *selectorRaw = nullptr;
    auto          app         = addApplication(session, local, std::move(selector), &selectorRaw);
    app->markReplaceInProgress();

    QDomDocument doc;
    const bool   ok
        = session.updateFromXml(J::Action::TransportReject, duplicatePayload(doc, QStringLiteral("duplicate")));

    check(!ok, "duplicate transport-reject contents were accepted");
    check(app->transport().data() == local.data() && app->replaceInProgress(),
          "duplicate transport-reject changed current replacement before rejection");
    check(selectorRaw->getNextCalls == 0 && selectorRaw->replaceCalls == 0,
          "duplicate transport-reject started fallback recovery before rejection");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    const auto args = application.arguments();
    check(args.size() == 2, "expected one transport-replace protocol case name");
    const auto test = args.at(1);

    if (test == QLatin1String("transport-reject"))
        testTransportRejectSelectsFallback(client);
    else if (test == QLatin1String("failed-iq-fallback"))
        testFailedIqSelectsFallback(client);
    else if (test == QLatin1String("malformed-accept"))
        testMalformedTransportAcceptRejectedAtomically(client);
    else if (test == QLatin1String("duplicate-accept"))
        testDuplicateTransportAcceptRejectedBeforeMutation(client);
    else if (test == QLatin1String("duplicate-reject"))
        testDuplicateTransportRejectRejectedBeforeRecovery(client);
    else
        qFatal("unknown transport-replace protocol case: %s", qPrintable(test));

    qInfo() << "Transport-replace protocol case passed:" << test;
    return 0;
}
