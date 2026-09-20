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
    void           setMarkUnackedOnTake(bool value) { markUnackedOnTake_ = value; }
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

    bool hasUpdates() const override { return hasUpdates_; }

    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement) override
    {
        if (!hasUpdates_ && !ensureTransportElement)
            return {};

        auto el = pad()->doc()->createElementNS(pad()->ns(), QStringLiteral("transport"));
        el.setAttribute(QStringLiteral("id"), id_);
        hasUpdates_ = false;

        // ICE transport signaling does not use Transport::State as the IQ
        // transaction lifetime. Keep that behavior available in this test
        // double so transport-replace arbitration cannot accidentally rely on
        // State::Unacked.
        if (markUnackedOnTake_)
            setState(J::State::Unacked);

        return { el, [self = QPointer<TestTransport>(this), markUnacked = markUnackedOnTake_](Task *task) {
                    if (self && markUnacked && task && task->success())
                        self->setState(J::State::Pending);
                } };
    }

    bool                      isValid() const override { return true; }
    J::TransportFeatures      features() const override { return J::TransportFeature::Reliable; }
    J::Connection::Ptr        addChannel(J::TransportFeatures, const QString &, int) override { return {}; }
    QList<J::Connection::Ptr> channels() const override { return {}; }

private:
    QString id_;
    bool    hasUpdates_        = false;
    bool    markUnackedOnTake_ = false;
    int     starts_            = 0;
};

class TestTransportManager : public J::TransportManager {
public:
    static QString namespaceUri() { return QStringLiteral("urn:iris:test:transport-replace-correctness"); }

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
    QSharedPointer<J::Transport> getNextTransport() override { return {}; }
    QSharedPointer<J::Transport> getAlikeTransport(QSharedPointer<J::Transport>) override { return {}; }
    bool                         replace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) override
    {
        ++replaceCalls;
        return true;
    }
    void backupTransport(QSharedPointer<J::Transport>) override { }
    bool hasMoreTransports() const override { return false; }
    bool hasTransport(QSharedPointer<J::Transport>) const override { return true; }
    bool canReplace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) override
    {
        ++canReplaceCalls;
        return true;
    }
    int compare(QSharedPointer<J::Transport>, QSharedPointer<J::Transport>) const override { return 1; }

    int canReplaceCalls = 0;
    int replaceCalls    = 0;
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

    void markReplacePlanned() { _pendingTransportReplace = PendingTransportReplace::Planned; }
    void markReplaceInProgress() { _pendingTransportReplace = PendingTransportReplace::InProgress; }
    bool replaceNeedAck() const { return _pendingTransportReplace == PendingTransportReplace::NeedAck; }
    bool replaceInProgress() const { return _pendingTransportReplace == PendingTransportReplace::InProgress; }

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
                                                   const QString &id, bool markUnackedOnTake = false)
{
    auto pad = session.transportPadFactory(TestTransportManager::namespaceUri());
    check(bool(pad), "test transport pad missing");
    auto transport = QSharedPointer<TestTransport>::create(pad, creator, id);
    transport->forceState(state);
    transport->setMarkUnackedOnTake(markUnackedOnTake);
    return transport;
}

static TestApplication *addApplication(J::Session &session, const QSharedPointer<TestTransport> &transport,
                                       std::unique_ptr<TestSelector> selector, TestSelector **selectorOut = nullptr,
                                       J::State state = J::State::Connecting)
{
    auto app = new TestApplication(&session, QStringLiteral("audio"), session.role());
    auto raw = app->installTransport(transport, std::move(selector));
    app->setState(state);
    session.addContent(app);
    if (selectorOut)
        *selectorOut = raw;
    return app;
}

static QDomElement makeReplace(QDomDocument &doc, const QList<QString> &ids)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    for (const auto &id : ids) {
        auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
        J::ContentBase::setCreatorAttr(content, J::Origin::Initiator);
        content.setAttribute(QStringLiteral("name"), QStringLiteral("audio"));
        auto transport = doc.createElementNS(TestTransportManager::namespaceUri(), QStringLiteral("transport"));
        transport.setAttribute(QStringLiteral("id"), id);
        content.appendChild(transport);
        jingle.appendChild(content);
    }
    return jingle;
}

static QDomElement makeReplaceWithoutTransport(QDomDocument &doc)
{
    auto jingle = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    auto content = doc.createElementNS(J::NS, QStringLiteral("content"));
    J::ContentBase::setCreatorAttr(content, J::Origin::Initiator);
    content.setAttribute(QStringLiteral("name"), QStringLiteral("audio"));
    jingle.appendChild(content);
    return jingle;
}

static bool isTieBreak(const J::Session &session)
{
    const auto error = session.lastError();
    return error && J::ErrorUtil::jingleCondition(*error) == J::ErrorUtil::TieBreak;
}

static void testInFlightReplaceWinsWithoutTransportUnacked(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       local
        = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("local-replacement"));
    local->setHasUpdates(true);
    auto app = addApplication(session, local, std::make_unique<TestSelector>());
    app->markReplacePlanned();

    check(app->evaluateOutgoingUpdate().action == J::Action::TransportReplace,
          "planned replacement did not evaluate to transport-replace");
    auto outgoing = app->takeOutgoingUpdate();
    auto xml      = client.doc()->createElementNS(J::NS, QStringLiteral("jingle"));
    for (const auto &element : std::get<0>(outgoing))
        xml.appendChild(element);
    session.tieBreaker()->outgoingStarted(J::Action::TransportReplace, xml);
    check(bool(std::get<1>(outgoing)) && app->replaceNeedAck(), "outgoing transport-replace is not in flight");
    check(local->state() == J::State::ApprovedToSend,
          "ICE-like test transport unexpectedly encoded IQ lifetime in transport state");

    QDomDocument doc;
    const bool   ok = session.updateFromXml(J::Action::TransportReplace,
                                            makeReplace(doc, { QStringLiteral("remote-replacement") }));
    check(!ok && isTieBreak(session),
          "initiator accepted a crossed transport-replace because Transport::State was not Unacked");
    check(app->transport().data() == local.data(), "crossed transport-replace replaced the initiator winner");
}

static void testUnackedTransportWithoutReplaceDoesNotTieBreak(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto local = makeTransport(session, J::Origin::Initiator, J::State::Unacked, QStringLiteral("unrelated-unacked"));
    auto app   = addApplication(session, local, std::make_unique<TestSelector>());

    QDomDocument doc;
    const bool   ok = session.updateFromXml(J::Action::TransportReplace,
                                            makeReplace(doc, { QStringLiteral("remote-replacement") }));
    check(ok && !isTieBreak(session),
          "Transport::State::Unacked was mistaken for an in-flight transport-replace transaction");
    check(app->transport().data() != local.data(), "valid peer transport-replace was not installed");
}

static void testFailedOutgoingReplaceLeavesNeedAck(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       local
        = makeTransport(session, J::Origin::Initiator, J::State::ApprovedToSend, QStringLiteral("local-replacement"));
    local->setHasUpdates(true);
    auto app = addApplication(session, local, std::make_unique<TestSelector>());
    app->markReplacePlanned();

    check(app->evaluateOutgoingUpdate().action == J::Action::TransportReplace,
          "planned replacement did not evaluate to transport-replace");
    auto update = app->takeOutgoingUpdate();
    check(app->replaceNeedAck(), "transport-replace did not enter NeedAck before failed IQ");
    const auto &ack = std::get<1>(update);
    check(bool(ack), "transport-replace had no IQ completion callback");
    Result failure(client.rootTask(), false);
    ack(&failure);

    check(!app->replaceNeedAck(), "failed transport-replace IQ left application permanently in NeedAck");
}

static void testMalformedTransportAcceptDoesNotFinishReplace(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto local = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("local-replacement"));
    auto app   = addApplication(session, local, std::make_unique<TestSelector>());
    app->markReplaceInProgress();

    QDomDocument doc;
    auto         transport = doc.createElementNS(TestTransportManager::namespaceUri(), QStringLiteral("transport"));
    transport.setAttribute(QStringLiteral("id"), QStringLiteral("malformed-accept"));
    transport.setAttribute(QStringLiteral("parse"), QStringLiteral("fail"));
    app->incomingTransportAccept(transport);

    check(app->replaceInProgress(), "malformed transport-accept silently completed transport replacement");
    check(local->starts() == 0, "malformed transport-accept started replacement transport");
}

static void testDuplicateContentReplaceIsAtomic(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       local    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("current"));
    auto       selector = std::make_unique<TestSelector>();
    TestSelector *selectorRaw = nullptr;
    auto          app         = addApplication(session, local, std::move(selector), &selectorRaw, J::State::Active);

    QDomDocument doc;
    const bool   ok = session.updateFromXml(J::Action::TransportReplace,
                                            makeReplace(doc, { QStringLiteral("first"), QStringLiteral("second") }));

    check(!ok, "duplicate (creator,name) transport-replace entries were accepted");
    check(app->transport().data() == local.data(), "duplicate transport-replace partially mutated current transport");
    check(selectorRaw->replaceCalls == 0, "duplicate transport-replace reached selector before validation completed");
}

static void testMissingTransportReplaceIsMalformed(Client &client)
{
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto       local    = makeTransport(session, J::Origin::Initiator, J::State::Pending, QStringLiteral("current"));
    auto       selector = std::make_unique<TestSelector>();
    TestSelector *selectorRaw = nullptr;
    auto          app         = addApplication(session, local, std::move(selector), &selectorRaw, J::State::Active);

    QDomDocument doc;
    const bool   ok = session.updateFromXml(J::Action::TransportReplace, makeReplaceWithoutTransport(doc));

    check(!ok, "transport-replace without a transport element was accepted as an unsupported transport");
    check(app->transport().data() == local.data(), "malformed transport-replace changed current transport");
    check(selectorRaw->canReplaceCalls == 0 && selectorRaw->replaceCalls == 0,
          "malformed transport-replace reached selector");
}

int main(int argc, char **argv)
{
    QCoreApplication     application(argc, argv);
    QCA::Initializer     qca;
    TestTransportManager transportManager;
    Client               client;
    client.jingleManager()->registerTransport(&transportManager);

    const auto args = application.arguments();
    check(args.size() == 2, "expected one transport-replace correctness case name");
    const auto test = args.at(1);

    if (test == QLatin1String("inflight-without-unacked"))
        testInFlightReplaceWinsWithoutTransportUnacked(client);
    else if (test == QLatin1String("unacked-without-replace"))
        testUnackedTransportWithoutReplaceDoesNotTieBreak(client);
    else if (test == QLatin1String("failed-iq"))
        testFailedOutgoingReplaceLeavesNeedAck(client);
    else if (test == QLatin1String("malformed-accept"))
        testMalformedTransportAcceptDoesNotFinishReplace(client);
    else if (test == QLatin1String("duplicate-content"))
        testDuplicateContentReplaceIsAtomic(client);
    else if (test == QLatin1String("missing-transport"))
        testMissingTransportReplaceIsMalformed(client);
    else
        qFatal("unknown transport-replace correctness case: %s", qPrintable(test));

    qInfo() << "Transport-replace correctness case passed:" << test;
    return 0;
}
