// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-s5b.cpp"
#include "../../src/xmpp/xmpp-im/jingle-ibb.h"
#include "../../src/xmpp/xmpp-im/jingle-application.h"

#include <QCoreApplication>
#include <QtCrypto>

using namespace XMPP;
namespace J = XMPP::Jingle;
namespace S = XMPP::Jingle::S5B;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

struct OwnedXml {
    QDomDocument doc;
    QDomElement  root;

    operator QDomElement() const { return root; }
    QDomElement firstChildElement() const { return root.firstChildElement(); }
};

static OwnedXml payload(const QString &children)
{
    OwnedXml xml;
    check(bool(xml.doc.setContent(QStringLiteral("<transport xmlns='urn:xmpp:jingle:transports:s5b:1' sid='test'>")
                                      + children + QStringLiteral("</transport>"),
                                  true)),
          "Invalid test XML");
    xml.root = xml.doc.documentElement();
    return xml;
}

namespace XMPP { namespace Jingle { namespace S5B {
    struct TransportTestAccess {
        static Candidate install(Transport &t, bool local, Candidate::State state, bool proxy = true, bool used = false)
        {
            auto el = payload(QStringLiteral("<candidate cid='chosen' host='127.0.0.1' port='54321'"
                                             " priority='100' jid='proxy.example.test' type='%1'/>")
                                  .arg(proxy ? QStringLiteral("proxy") : QStringLiteral("direct")))
                          .firstChildElement();
            Candidate candidate(&t, el);
            candidate.setState(state);
            auto &map            = local ? t.d->localCandidates : t.d->remoteCandidates;
            map[candidate.cid()] = candidate;
            if (used)
                (local ? t.d->localUsedCandidate : t.d->remoteUsedCandidate) = candidate;
            t._state = ::XMPP::Jingle::State::Connecting;
            return candidate;
        }
        static Candidate nominate(Transport &t, bool local, Candidate::State state, bool proxy = true)
        {
            return install(t, local, state, proxy, true);
        }
        static QString usedCid(const Transport &t, bool local)
        {
            return (local ? t.d->localUsedCandidate : t.d->remoteUsedCandidate).cid();
        }
        static bool remoteReportedCandidateError(const Transport &t) { return t.d->remoteReportedCandidateError; }
        static void outgoingProxyError(Transport &t)
        {
            t.d->offerSent      = true;
            t.d->pendingActions = Transport::Private::ProxyError;
        }
        static size_t remoteCount(const Transport &t) { return t.d->remoteCandidates.size(); }
    };
}}}

class QueueSelector final : public J::TransportSelector {
public:
    explicit QueueSelector(QList<QSharedPointer<J::Transport>> transports) : transports_(std::move(transports)) { }

    QSharedPointer<J::Transport> getNextTransport() override
    {
        return transports_.isEmpty() ? QSharedPointer<J::Transport>() : transports_.takeFirst();
    }

    QSharedPointer<J::Transport> getAlikeTransport(QSharedPointer<J::Transport> alike) override
    {
        if (!alike)
            return {};
        for (qsizetype i = 0; i < transports_.size(); ++i) {
            if (transports_[i] && transports_[i]->pad()->ns() == alike->pad()->ns())
                return transports_.takeAt(i);
        }
        return {};
    }

    void backupTransport(QSharedPointer<J::Transport> transport) override
    {
        if (transport)
            transports_.prepend(std::move(transport));
    }

    bool hasMoreTransports() const override { return !transports_.isEmpty(); }
    bool hasTransport(QSharedPointer<J::Transport> transport) const override { return transports_.contains(transport); }
    int compare(QSharedPointer<J::Transport> a, QSharedPointer<J::Transport> b) const override
    {
        return a == b ? 0 : a ? (b ? 0 : 1) : -1;
    }
    bool replace(QSharedPointer<J::Transport>, QSharedPointer<J::Transport> newer) override { return bool(newer); }

private:
    QList<QSharedPointer<J::Transport>> transports_;
};

class TestApplicationPad final : public J::ApplicationManagerPad {
public:
    explicit TestApplicationPad(J::Session *session) : session_(session) { }

    QString ns() const override { return QStringLiteral("urn:test:s5b-fallback"); }
    J::Session *session() const override { return session_; }
    J::ApplicationManager *manager() const override { return nullptr; }
    QString generateContentName(J::Origin) override { return QStringLiteral("fallback"); }

private:
    J::Session *session_;
};

class FallbackApplication final : public J::Application {
public:
    FallbackApplication(const QSharedPointer<TestApplicationPad> &pad, QList<QSharedPointer<J::Transport>> transports)
    {
        _pad               = pad;
        _contentName       = QStringLiteral("fallback");
        _creator           = pad->session()->role();
        _senders           = J::Origin::Both;
        _transportSelector = std::make_unique<QueueSelector>(std::move(transports));
    }

    void setState(J::State state) override { _state = state; }
    const std::optional<XMPP::Stanza::Error> &lastError() const override { return error_; }
    J::Reason lastReason() const override { return reason_; }
    SetDescError setRemoteOffer(const QDomElement &) override { return Ok; }
    SetDescError setRemoteAnswer(const QDomElement &) override { return Ok; }
    void prepare() override { }
    void start() override { }
    void remove(J::Reason::Condition condition = J::Reason::Success, const QString &text = QString()) override
    {
        reason_ = J::Reason(condition, text);
        _state  = J::State::Finished;
    }

protected:
    QDomElement makeLocalOffer() override { return {}; }
    QDomElement makeLocalAnswer() override { return {}; }
    void incomingRemove(const J::Reason &reason) override
    {
        reason_ = reason;
        _state  = J::State::Finished;
    }
    void prepareTransport() override { }

private:
    std::optional<XMPP::Stanza::Error> error_;
    J::Reason                          reason_;
};

struct Fixture {
    J::Session                   session;
    S::Manager                   manager;
    J::TransportManagerPad::Ptr  pad;
    QSharedPointer<S::Transport> transport;

    Fixture(Client &client, J::Origin role = J::Origin::Responder) :
        session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), role)
    {
        // This fixture manager is intentionally not registered with the client's
        // Jingle manager. Binding it manually would make its destructor unregister
        // the client's built-in S5B manager.
        pad       = J::TransportManagerPad::Ptr(manager.pad(&session));
        transport = manager.newTransport(pad, session.peerRole()).staticCast<S::Transport>();
    }
};

static void testMissingPortScope(Client &client)
{
    Fixture f(client);
    int failures = 0;
    QObject::connect(f.transport.data(), &J::Transport::failed, &f.session, [&] { ++failures; });
    f.transport->prepare();
    check(f.transport->state() == J::State::Finished, "Missing S5B port scope did not finish transport");
    check(failures == 1, "Missing S5B port scope did not report exactly one transport failure");
    check(f.transport->lastReason().condition() == J::Reason::GeneralError,
          "Missing S5B port scope reported the wrong reason");
}

static void testPreparation(Client &client)
{
    Fixture    f(client);
    const auto candidate = QStringLiteral("<candidate cid='a' host='127.0.0.1' jid='peer.example.test'"
                                          " port='54321' priority='100' type='direct'/>");
    for (const auto &contents :
         { candidate + QStringLiteral("<candidate-used/>"), QStringLiteral("<candidate-error/>") + candidate,
           QStringLiteral("<candidate-error/><candidate-error/>"), QStringLiteral("<activated cid='a'/><proxy-error/>"),
           candidate + QStringLiteral("<candidate cid='broken'/>"),
           QStringLiteral("<proxy-error><candidate-error/></proxy-error>") }) {
        check(!f.transport->prepareUpdate(payload(contents)), "Malformed payload was staged");
        check(f.transport->sid().isEmpty() && S::TransportTestAccess::remoteCount(*f.transport) == 0
                  && f.transport->state() == J::State::Created,
              "Rejected preparation mutated live transport");
    }
    auto prepared = f.transport->prepareUpdate(payload(candidate));
    check(bool(prepared) && S::TransportTestAccess::remoteCount(*f.transport) == 0,
          "Valid preparation has side effects");
    check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "Valid commit failed");
    check(f.transport->sid() == QLatin1String("test") && S::TransportTestAccess::remoteCount(*f.transport) == 1,
          "Valid commit did not install candidates");

    check(bool(f.transport->prepareUpdate(payload(QStringLiteral("<candidate-used xmlns='urn:unknown'/>")))),
          "Foreign extension was interpreted as an S5B command");
}

static void testValidCommands(Client &client)
{
    {
        Fixture f(client);
        auto candidate = S::TransportTestAccess::install(*f.transport, true, S::Candidate::Pending);
        auto prepared  = f.transport->prepareUpdate(payload(QStringLiteral("<candidate-used cid='chosen'/>")));
        check(bool(prepared) && candidate.state() == S::Candidate::Pending,
              "candidate-used preparation mutated or rejected valid state");
        check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "candidate-used commit failed");
        check(candidate.state() == S::Candidate::Accepted
                  && S::TransportTestAccess::usedCid(*f.transport, true) == QLatin1String("chosen"),
              "candidate-used did not select the nominated local proxy");
    }

    {
        Fixture f(client);
        auto candidate = S::TransportTestAccess::install(*f.transport, true, S::Candidate::Pending, false);
        auto prepared  = f.transport->prepareUpdate(payload(QStringLiteral("<candidate-error/>")));
        check(bool(prepared) && candidate.state() == S::Candidate::Pending
                  && !S::TransportTestAccess::remoteReportedCandidateError(*f.transport),
              "candidate-error preparation mutated valid state");
        check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "candidate-error commit failed");
        check(candidate.state() == S::Candidate::Discarded
                  && S::TransportTestAccess::remoteReportedCandidateError(*f.transport),
              "candidate-error did not discard local pending candidates");
    }

    {
        Fixture f(client);
        auto candidate = S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted);
        auto prepared  = f.transport->prepareUpdate(payload(QStringLiteral("<activated cid='chosen'/>")));
        check(bool(prepared) && candidate.state() == S::Candidate::Accepted,
              "activated preparation mutated or rejected valid proxy state");
        check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "activated commit failed");
        check(candidate.state() == S::Candidate::Active, "activated did not activate the selected remote proxy");
    }
}

static void testProxyError(Client &client, J::Origin role, bool local, S::Candidate::State state)
{
    Fixture f(client, role);
    auto    candidate = S::TransportTestAccess::nominate(*f.transport, local, state);
    int     failures  = 0;
    QObject::connect(f.transport.data(), &J::Transport::failed, &f.session, [&] { ++failures; });
    const auto xml      = payload(QStringLiteral("<proxy-error/>"));
    auto       prepared = f.transport->prepareUpdate(xml);
    check(bool(prepared) && candidate.state() == state && !failures, "Proxy error preparation failed or mutated state");
    check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "Proxy error commit failed");
    check(f.transport->state() == J::State::Finishing && candidate.state() == S::Candidate::Discarded,
          "Proxy negotiation was not retired");
    check(!f.transport->update(xml), "Duplicate proxy error was accepted");
    QCoreApplication::processEvents();
    check(failures == 1 && f.transport->state() == J::State::Finished, "Proxy error did not signal fallback once");
    QCoreApplication::processEvents();
    check(failures == 1, "Proxy error signaled duplicate failure");
}

static void testProxyErrorFallsBackToIbb(Client &client)
{
    Fixture f(client, J::Origin::Initiator);

    J::IBB::Manager ibbManager;
    // As with the S5B fixture manager, this local manager is not registered in
    // the client's transport registry and must not unregister the built-in IBB manager.
    J::TransportManagerPad::Ptr ibbPad(ibbManager.pad(&f.session));
    auto ibb = ibbManager.newTransport(ibbPad, f.session.role());
    check(bool(ibb), "Could not create real IBB fallback transport");

    auto appPad = QSharedPointer<TestApplicationPad>::create(&f.session);
    FallbackApplication app(appPad, { f.transport, ibb });
    check(app.selectNextTransport() && app.transport() == f.transport,
          "Application did not select the S5B transport first");

    auto candidate = S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted);
    auto prepared  = f.transport->prepareUpdate(payload(QStringLiteral("<proxy-error/>")));
    check(bool(prepared), "Could not stage proxy-error for fallback");
    check(f.transport->commitPreparedUpdate(std::move(prepared.update)), "Could not commit proxy-error for fallback");

    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    check(candidate.state() == S::Candidate::Discarded, "Failed proxy remained nominated during fallback");
    check(app.transport() && app.transport() == ibb && app.transport()->pad()->ns() == J::IBB::NS,
          "S5B proxy failure did not select the real IBB fallback transport");
}

static void testStaleAndUnexpected(Client &client)
{
    Fixture    f(client);
    const auto xml = payload(QStringLiteral("<proxy-error/>"));
    check(!f.transport->prepareUpdate(xml), "Proxy error without nomination accepted");
    S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted, false);
    check(!f.transport->prepareUpdate(xml), "Proxy error terminated direct candidate");
    S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted);
    auto prepared = f.transport->prepareUpdate(xml);
    check(bool(prepared), "Could not stage proxy error");
    auto newer = S::TransportTestAccess::nominate(*f.transport, false, S::Candidate::Accepted);
    check(!f.transport->commitPreparedUpdate(std::move(prepared.update)), "Stale same-cid snapshot was committed");
    check(newer.state() == S::Candidate::Accepted, "Stale proxy error mutated successor");
}

class Success final : public Task {
public:
    explicit Success(Task *parent) : Task(parent) { setSuccess(); }
};

static void testOutgoingError(Client &client, S::Candidate::State state, bool crossed)
{
    Fixture f(client);
    S::TransportTestAccess::nominate(*f.transport, true, state);
    S::TransportTestAccess::outgoingProxyError(*f.transport);
    int failures = 0;
    QObject::connect(f.transport.data(), &J::Transport::failed, &f.session, [&] { ++failures; });
    auto [xml, ack] = f.transport->takeOutgoingUpdate(false);
    check(!xml.firstChildElement(QStringLiteral("proxy-error")).isNull() && bool(ack), "Missing outgoing proxy error");
    if (crossed)
        check(f.transport->update(payload(QStringLiteral("<proxy-error/>"))), "Crossed proxy error rejected");
    Success success(client.rootTask());
    ack(&success);
    ack(&success);
    QCoreApplication::processEvents();
    check(failures == 1 && f.transport->state() == J::State::Finished,
          "Proxy-error ACK did not finish exact attempt once");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    TcpPortReserver  reserver;
    client.setTcpPortReserver(&reserver);
    testMissingPortScope(client);
    testPreparation(client);
    testValidCommands(client);
    testProxyErrorFallsBackToIbb(client);
    for (auto role : { J::Origin::Initiator, J::Origin::Responder }) {
        testProxyError(client, role, false, S::Candidate::Accepted);
        testProxyError(client, role, true, S::Candidate::Accepted);
        testProxyError(client, role, true, S::Candidate::Activating);
    }
    testStaleAndUnexpected(client);
    testOutgoingError(client, S::Candidate::Activating, false);
    testOutgoingError(client, S::Candidate::Discarded, false);
    testOutgoingError(client, S::Candidate::Activating, true);
    qInfo("S5B staged payload and proxy failure regressions passed");
}
