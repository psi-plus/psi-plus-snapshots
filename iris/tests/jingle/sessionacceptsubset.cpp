// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QSharedPointer>

#include <iris/jingle-application.h>
#include <iris/xmpp_client.h>
#include <qca.h>

// Exercise the same parser entry point that JTPush calls, without network I/O.
#define private public
#include <iris/jingle-session.h>
#undef private

#include <functional>
#include <utility>

using namespace XMPP;
using namespace XMPP::Jingle;

static const QString applicationNs = QStringLiteral("urn:iris:test:subset-application");
static const QString transportNs   = QStringLiteral("urn:iris:test:subset-transport");

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static void pump()
{
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

struct Stats {
    int                   starts  = 0;
    int                   removes = 0;
    int                   stops   = 0;
    std::function<void()> onStop;
    std::function<void()> onRemove;
    std::function<void()> onStart;
};

class TestApplicationPad final : public ApplicationManagerPad {
public:
    explicit TestApplicationPad(Session *session) : session_(session) { }

    Session            *session() const override { return session_; }
    QString             ns() const override { return applicationNs; }
    ApplicationManager *manager() const override { return nullptr; }
    QString             generateContentName(Origin) override { return {}; }

private:
    Session *session_ = nullptr;
};

class TestTransportPad final : public TransportManagerPad {
public:
    explicit TestTransportPad(Session *session) : session_(session) { }

    Session          *session() const override { return session_; }
    QString           ns() const override { return transportNs; }
    TransportManager *manager() const override { return nullptr; }

private:
    Session *session_ = nullptr;
};

class TestTransport final : public Transport {
public:
    TestTransport(Session *session, Origin creator, const QSharedPointer<Stats> &stats) :
        Transport(TransportManagerPad::Ptr(new TestTransportPad(session)), creator), stats_(stats)
    {
    }

    void prepare() override { setState(State::ApprovedToSend); }
    void start() override { setState(State::Active); }
    void stop() override
    {
        ++stats_->stops;
        Transport::stop();
        auto callback = std::move(stats_->onStop);
        if (callback)
            callback();
    }
    bool update(const QDomElement &) override
    {
        setState(State::Accepted);
        return true;
    }
    bool                        hasUpdates() const override { return false; }
    OutgoingTransportInfoUpdate takeOutgoingUpdate(bool = false) override { return {}; }
    bool                        isValid() const override { return true; }
    TransportFeatures           features() const override { return TransportFeature::Reliable; }
    Connection::Ptr             addChannel(TransportFeatures, const QString &, int = -1) override { return {}; }
    QList<Connection::Ptr>      channels() const override { return {}; }

private:
    QSharedPointer<Stats> stats_;
};

class TestApplication final : public Application {
public:
    TestApplication(Session *session, QString name, const QSharedPointer<Stats> &stats) : stats_(stats)
    {
        _pad         = ApplicationManagerPad::Ptr(new TestApplicationPad(session));
        _contentName = std::move(name);
        _creator     = Origin::Initiator;
        _senders     = Origin::Both;
        _state       = State::Pending;
        _flags |= InitialApplication;
        _transport = QSharedPointer<TestTransport>::create(session, Origin::Initiator, stats_);
    }

    void                                setState(State state) override { _state = state; }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    Reason                              lastReason() const override { return reason_; }

    SetDescError setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError setRemoteAnswer(const QDomElement &) override
    {
        setState(State::Accepted);
        return Ok;
    }
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    Update      evaluateOutgoingUpdate() override { return { Action::NoAction, {} }; }
    void        prepare() override { }
    void        start() override
    {
        ++stats_->starts;
        setState(State::Active);
        auto callback = std::move(stats_->onStart);
        if (callback)
            callback();
    }
    void remove(Reason::Condition condition = Reason::Success, const QString &text = {}) override
    {
        reason_ = Reason(condition, text);
        setState(State::Finished);
    }
    void incomingRemove(const Reason &reason) override
    {
        reason_ = reason;
        ++stats_->removes;
        setState(State::Finished);
        auto callback = std::move(stats_->onRemove);
        if (callback)
            callback();
    }

protected:
    void prepareTransport() override { }

private:
    QSharedPointer<Stats>        stats_;
    std::optional<Stanza::Error> error_;
    Reason                       reason_;
};

struct OwnedXml {
    QDomDocument doc;
    QDomElement  root;

    operator QDomElement() const { return root; }
    QDomElement firstChildElement() const { return root.firstChildElement(); }
    QDomDocument ownerDocument() const { return root.ownerDocument(); }
    QDomNode appendChild(const QDomNode &node) { return root.appendChild(node); }
};

static OwnedXml emptyAnswer()
{
    OwnedXml xml;
    xml.root = xml.doc.createElementNS(NS, QStringLiteral("jingle"));
    return xml;
}

static OwnedXml answer(Application *accepted, bool malformed = false)
{
    OwnedXml xml;
    auto    &doc     = xml.doc;
    auto     jingle  = doc.createElementNS(NS, QStringLiteral("jingle"));
    auto         content = doc.createElementNS(NS, QStringLiteral("content"));
    content.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
    content.setAttribute(QStringLiteral("name"), accepted->contentName());
    content.appendChild(doc.createElementNS(applicationNs, QStringLiteral("description")));
    content.appendChild(doc.createElementNS(transportNs, QStringLiteral("transport")));
    jingle.appendChild(content);

    if (malformed) {
        auto missing = doc.createElementNS(NS, QStringLiteral("content"));
        missing.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
        missing.setAttribute(QStringLiteral("name"), QStringLiteral("missing"));
        jingle.appendChild(missing);
    }
    xml.root = jingle;
    return xml;
}

static void addInitialPair(Session &session, const QSharedPointer<Stats> &stats, TestApplication **audio,
                           TestApplication **video)
{
    *audio = new TestApplication(&session, QStringLiteral("audio"), stats);
    *video = new TestApplication(&session, QStringLiteral("video"), stats);
    session.addContent(*audio);
    session.addContent(*video);
}

static void addInitialTriple(Session &session, const QSharedPointer<Stats> &stats, TestApplication **audio,
                             TestApplication **video, TestApplication **zvideo)
{
    addInitialPair(session, stats, audio, video);
    *zvideo = new TestApplication(&session, QStringLiteral("zvideo"), stats);
    session.addContent(*zvideo);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;

    // Reject a session-accept that omits every still-pending initial content.
    // This is a validation failure, so it must not commit any subset cleanup.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> audioGuard(audio);
        QPointer<TestApplication> videoGuard(video);
        const auto                initialState = session.state();
        int                       activations  = 0;
        QObject::connect(&session, &Session::activated, &session, [&activations]() { ++activations; });

        check(!session.updateFromXml(Action::SessionAccept, emptyAnswer()),
              "empty session-accept that rejected every initial content was accepted");
        check(session.state() == initialState, "empty session-accept changed session state");
        check(audioGuard && videoGuard && audioGuard->state() == State::Pending
                  && videoGuard->state() == State::Pending,
              "empty session-accept changed or removed initial contents");
        check(session.content(QStringLiteral("audio"), Origin::Initiator) == audioGuard.data()
                  && session.content(QStringLiteral("video"), Origin::Initiator) == videoGuard.data(),
              "empty session-accept detached initial contents");
        check(stats->removes == 0 && stats->stops == 0 && stats->starts == 0,
              "empty session-accept performed content side effects");
        pump();
        check(activations == 0, "empty session-accept activated the session");
    }

    // Full validation must finish before any omitted-content cleanup is committed.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);
        int                       activations = 0;
        QObject::connect(&session, &Session::activated, &session, [&activations]() { ++activations; });

        check(!session.updateFromXml(Action::SessionAccept, answer(audio, true)),
              "malformed subset session-accept was accepted");
        check(audio->state() == State::Pending, "malformed answer did not roll accepted content back");
        check(videoGuard && videoGuard->state() == State::Pending
                  && session.content(QStringLiteral("video"), Origin::Initiator) == videoGuard.data(),
              "malformed subset answer removed omitted initial content");
        check(stats->removes == 0 && stats->stops == 0, "malformed answer performed content cleanup");

        check(session.updateFromXml(Action::SessionAccept, answer(audio)), "valid subset session-accept was rejected");
        check(session.content(QStringLiteral("audio"), Origin::Initiator) == audio,
              "subset session-accept removed accepted initial content");
        check(!videoGuard && !session.content(QStringLiteral("video"), Origin::Initiator),
              "subset session-accept retained omitted initial content");
        check(stats->removes == 1 && stats->stops == 1, "omitted initial content was not cleaned up exactly once");
        pump();
        check(session.state() == State::Active && audio->state() == State::Active && stats->starts == 1,
              "accepted subset content did not start normally");
        check(activations == 1, "normal subset session-accept did not activate exactly once");
    }

    // A reentrant local termination from Transport::stop() wins over the
    // incoming session-accept. The handler must not restore Active afterwards.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);
        stats->onStop = [&session]() { session.terminate(Reason::Decline, QStringLiteral("test cancellation")); };

        check(session.updateFromXml(Action::SessionAccept, answer(audio)),
              "session-accept was rejected after reentrant termination");
        check(session.state() == State::Finishing, "session-accept overwrote a reentrant termination");
        check(!videoGuard, "detached omitted content leaked after reentrant termination");
        check(stats->starts == 0, "accepted content started after reentrant termination");
    }

    // The current omitted content is detached before stop(). If stop destroys
    // the whole Session, that detached object is no longer owned by the Session
    // destructor and must still be released by the handler.
    {
        auto stats = QSharedPointer<Stats>::create();
        auto session
            = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        QPointer<Session> sessionGuard(session);
        TestApplication  *audio = nullptr, *video = nullptr;
        addInitialPair(*session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);
        stats->onStop = [session]() { delete session; };

        check(session->updateFromXml(Action::SessionAccept, answer(audio)),
              "session-accept did not survive Session deletion from stop callback");
        check(!sessionGuard, "Session survived its stop callback deletion");
        check(!videoGuard, "detached omitted content leaked when stop callback deleted Session");
        check(stats->starts == 0, "accepted content started after Session deletion");
    }

    // A callback for one omitted content may destroy another omitted content.
    // Snapshotted QPointers must make the later cleanup entry harmless.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr, *zvideo = nullptr;
        addInitialTriple(session, stats, &audio, &video, &zvideo);
        QPointer<TestApplication> videoGuard(video);
        QPointer<TestApplication> zvideoGuard(zvideo);
        stats->onStop = [zvideo]() { delete zvideo; };

        check(session.updateFromXml(Action::SessionAccept, answer(audio)),
              "session-accept failed after neighboring omitted content was deleted");
        check(!videoGuard && !zvideoGuard, "neighboring omitted content cleanup left an object alive");
        pump();
        check(session.state() == State::Active && audio->state() == State::Active && stats->starts == 1,
              "neighbor deletion prevented the accepted content from starting");
    }

    // An omitted-content callback can also invalidate a content that was
    // accepted earlier in the same stanza. Never dereference the raw parser
    // result or activate a Session whose accepted content disappeared.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> audioGuard(audio);
        QPointer<TestApplication> videoGuard(video);
        stats->onStop = [audio]() { delete audio; };

        check(session.updateFromXml(Action::SessionAccept, answer(audio)),
              "session-accept failed after accepted content was invalidated reentrantly");
        check(!audioGuard && !videoGuard, "reentrant accepted-content deletion left stale content alive");
        check(session.state() == State::Finishing, "session activated after its accepted content disappeared");
        check(stats->starts == 0, "deleted accepted content was started");
    }

    // incomingRemove() is another external callback boundary. Deleting the
    // Session there must have the same detached-object lifetime guarantees.
    {
        auto stats = QSharedPointer<Stats>::create();
        auto session
            = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        QPointer<Session> sessionGuard(session);
        TestApplication  *audio = nullptr, *video = nullptr;
        addInitialPair(*session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);
        stats->onRemove = [session]() { delete session; };

        check(session->updateFromXml(Action::SessionAccept, answer(audio)),
              "session-accept did not survive Session deletion from incomingRemove callback");
        check(!sessionGuard, "Session survived its incomingRemove callback deletion");
        check(!videoGuard, "detached omitted content leaked when incomingRemove deleted Session");
        check(stats->starts == 0, "accepted content started after incomingRemove deleted Session");
    }

    // Ordinary content-accept must not inherit the initial-session subset rule.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        TestApplication *audio = nullptr, *video = nullptr;
        addInitialPair(session, stats, &audio, &video);
        QPointer<TestApplication> videoGuard(video);

        check(session.updateFromXml(Action::ContentAccept, answer(audio)), "ordinary content-accept was rejected");
        check(videoGuard && videoGuard->state() == State::Pending
                  && session.content(QStringLiteral("video"), Origin::Initiator) == videoGuard.data(),
              "content-accept incorrectly removed an omitted sibling");
        check(stats->removes == 0 && stats->stops == 0, "content-accept performed subset cleanup");
    }

    // Accepted siblings must progress independently across the queued start and
    // synchronous start callbacks. Session cancellation still stops the batch.
    for (int scenario = 0; scenario < 7; ++scenario) {
        auto stats = QSharedPointer<Stats>::create();
        auto session
            = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        QPointer<Session> guard(session);
        TestApplication  *audio = nullptr, *video = nullptr;
        addInitialPair(*session, stats, &audio, &video);
        QPointer<TestApplication> audioGuard(audio), videoGuard(video);
        int                       activations = 0;
        QObject::connect(session, &Session::activated, &app, [&]() { ++activations; });
        auto both   = answer(audio);
        auto second = answer(video);
        both.appendChild(both.ownerDocument().importNode(second.firstChildElement(), true));
        check(session->updateFromXml(Action::SessionAccept, both), "full acceptance failed");
        check(stats->starts == 0 && activations == 0, "start overtook acceptance ACK boundary");
        switch (scenario) {
        case 0:
            delete audio;
            break;
        case 1:
            stats->onStart = [audio]() { delete audio; };
            break;
        case 2:
            stats->onStart = [video]() { delete video; };
            break;
        case 3:
            delete audio;
            delete video;
            break;
        case 4:
            stats->onStart = [session]() { session->terminate(Reason::Cancel); };
            break;
        case 5:
            stats->onStart = [session]() { delete session; };
            break;
        case 6:
            stats->onStart = [audio, video]() {
                delete audio;
                delete video;
            };
            break;
        }
        pump();
        if (scenario <= 2) {
            check(guard && guard->state() == State::Active && activations == 1,
                  "surviving content did not activate session exactly once");
            check((audioGuard && audioGuard->state() == State::Active)
                      || (videoGuard && videoGuard->state() == State::Active),
                  "surviving accepted content was stranded");
            check(stats->starts == (scenario == 1 ? 2 : 1), "wrong surviving start count");
        } else {
            check(activations == 0, "cancelled or empty session activated");
            check(!guard || guard->state() >= State::Finishing, "empty/cancelled session stayed Active");
            check(stats->starts == (scenario == 3 ? 0 : 1), "batch continued after cancellation");
        }
        if (guard)
            delete guard.data();
    }

    // Later content-accept shares the scheduler but must not reactivate Session.
    {
        auto    stats = QSharedPointer<Stats>::create();
        Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Initiator);
        auto    audio = new TestApplication(&session, QStringLiteral("audio"), stats);
        session.addContent(audio);
        int activations = 0;
        QObject::connect(&session, &Session::activated, &session, [&]() { ++activations; });
        check(session.updateFromXml(Action::SessionAccept, answer(audio)), "initial acceptance failed");
        pump();
        auto video  = new TestApplication(&session, QStringLiteral("video"), stats);
        auto zvideo = new TestApplication(&session, QStringLiteral("zvideo"), stats);
        session.addContent(video);
        session.addContent(zvideo);
        auto both   = answer(video);
        auto second = answer(zvideo);
        both.appendChild(both.ownerDocument().importNode(second.firstChildElement(), true));
        check(session.updateFromXml(Action::ContentAccept, both), "later acceptance failed");
        delete video;
        check(stats->starts == 1, "later application started inline");
        pump();
        check(zvideo->state() == State::Active && stats->starts == 2, "later surviving content did not start");
        check(activations == 1, "content-accept reactivated Session");
    }

    qInfo("Jingle initial subset session-accept regressions passed");
}
