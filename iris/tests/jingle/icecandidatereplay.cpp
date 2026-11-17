// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QtCore>
#include <QtCrypto>
#include <QtNetwork>
#include <iris/jingle-application.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_xmlcommon.h>

// Inspect the real engine so replay cannot be hidden by the signaling cache.
#define private public
#include "../../src/irisnet/noncore/ice176.cpp"
#include "../../src/xmpp/xmpp-im/jingle-ice-pad_p.h"
#undef private

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class TestApplicationPad final : public J::ApplicationManagerPad {
public:
    explicit TestApplicationPad(J::Session *session) : J::ApplicationManagerPad(session), session_(session) { }
    J::Session            *session() const override { return session_; }
    QString                ns() const override { return QStringLiteral("urn:iris:test:bundle-runtime"); }
    J::ApplicationManager *manager() const override { return nullptr; }
    QString                generateContentName(J::Origin) override { return {}; }

private:
    J::Session *session_ = nullptr;
};

class TestTransportSelector final : public J::TransportSelector {
public:
    QPointer<J::Transport> getNextTransport() override { return {}; }
    QPointer<J::Transport> getAlikeTransport(QPointer<J::Transport>) override { return {}; }
    bool replace(QPointer<J::Transport>, QPointer<J::Transport> newer) override { return bool(newer); }
    void backupTransport(QPointer<J::Transport>) override { }
    bool hasMoreTransports() const override { return false; }
    bool hasTransport(QPointer<J::Transport>) const override { return false; }
    int  compare(QPointer<J::Transport>, QPointer<J::Transport>) const override { return 0; }
};

class TestApplication final : public J::Application {
public:
    TestApplication(J::ApplicationManagerPad *pad, QString name)
    {
        _pad               = pad;
        _contentName       = std::move(name);
        _creator           = J::Origin::Initiator;
        _senders           = J::Origin::Both;
        _transportSelector = std::make_unique<TestTransportSelector>();
    }

    void                                      setState(J::State state) override { _state = state; }
    const std::optional<XMPP::Stanza::Error> &lastError() const override { return error_; }
    J::Reason                                 lastReason() const override { return reason_; }
    SetDescError                              setRemoteOffer(const QDomElement &) override { return Ok; }
    SetDescError                              setRemoteAnswer(const QDomElement &) override { return Ok; }
    QDomElement                               makeLocalOffer() override { return {}; }
    QDomElement                               makeLocalAnswer() override { return {}; }
    void                                      prepare() override { }
    void                                      start() override { }
    bool                                      allowsSharedTransport() const override { return true; }
    void remove(J::Reason::Condition = J::Reason::Success, const QString & = {}) override { }
    void incomingRemove(const J::Reason &) override { }

protected:
    void prepareTransport() override { }

private:
    std::optional<XMPP::Stanza::Error> error_;
    J::Reason                          reason_;
};

static QDomElement update(QDomDocument &doc, const QString &ns, bool second = false)
{
    QString xml = QStringLiteral("<transport xmlns='%1' ufrag='peer' pwd='peer-password-for-ice-testing'>"
                                 "<candidate component='1' foundation='1' generation='0' id='first' "
                                 "ip='127.0.0.1' port='50101' priority='2130706431' protocol='udp' type='host'/>")
                      .arg(ns);
    if (second)
        xml += QStringLiteral("<candidate component='1' foundation='2' generation='0' id='second' "
                              "ip='127.0.0.1' port='50102' priority='2130706430' protocol='udp' type='host'/>");
    xml += QStringLiteral("</transport>");
    check(bool(XMLHelper::parseDocument(doc, xml, true)), "invalid candidate fixture");
    return doc.documentElement();
}

static bool deliver(J::ICE::Transport *transport, const QDomElement &xml)
{
    if (!transport->update(xml))
        return false;
    // Production commits transport updates on the next event-loop turn.
    QCoreApplication::processEvents();
    return true;
}

static void exercise(const QString &ns, bool candidatesBeforeStart)
{
    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    auto      *pad = qobject_cast<J::ICE::Pad *>(client.jingleManager()->transportPad(&session, ns));
    check(pad, "missing ICE pad");
    auto *appPad = new TestApplicationPad(&session);
    auto *first  = new TestApplication(appPad, QStringLiteral("first"));
    auto *second = new TestApplication(appPad, QStringLiteral("second"));
    session.addContent(first);
    session.addContent(second);
    auto *firstTransport  = new J::ICE::Transport(pad, J::Origin::Initiator);
    auto *secondTransport = new J::ICE::Transport(pad, J::Origin::Initiator);
    check(first->setTransport(firstTransport) && second->setTransport(secondTransport), "transport rejected");
    check(session.setGroupings(
              { J::ContentGroup { QStringLiteral("BUNDLE"), { QStringLiteral("first"), QStringLiteral("second") } } }),
          "group rejected");
    bool  bound = false, grouped = false;
    auto *network = pad->d->groupedConnectionFor(firstTransport, &bound, &grouped);
    check(network && bound && grouped, "first transport not grouped");
    check(pad->d->groupedConnectionFor(secondTransport, &bound, &grouped) == network && bound && grouped,
          "second transport did not share ICE");

    QDomDocument doc;
    const auto   initial = update(doc, ns);
    QDomDocument credentialsDoc;
    const auto   credentialsOnly = update(credentialsDoc, ns);
    while (!credentialsOnly.firstChild().isNull())
        credentialsDoc.documentElement().removeChild(credentialsOnly.firstChild());
    check(deliver(firstTransport, candidatesBeforeStart ? initial : credentialsOnly), "pre-start update rejected");
    check(!network->ice, "signaling unexpectedly started ICE");
    firstTransport->prepare();
    QElapsedTimer deadline;
    deadline.start();
    while ((!network->ice || network->ice->d->state != Ice176::Private::Started) && deadline.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    check(network->ice && network->ice->d->state == Ice176::Private::Started, "ICE did not start");
    auto *engine = network->ice->d;
    check(engine->remoteCandidates.size() == (candidatesBeforeStart ? 1 : 0), "pre-start candidates changed");
    check(engine->peerUser == QLatin1String("peer")
              && engine->peerPass == QLatin1String("peer-password-for-ice-testing"),
          "candidate-free startup lost ICE credentials");
    // A later credentials-only message must reach the engine even when there
    // are no new candidates. Reset only the engine copy to observe delivery.
    engine->peerUser.clear();
    engine->peerPass.clear();
    check(deliver(firstTransport, credentialsOnly), "credentials-only update rejected");
    check(engine->peerUser == QLatin1String("peer")
              && engine->peerPass == QLatin1String("peer-password-for-ice-testing"),
          "credentials-only update did not reach ICE");
    check(deliver(firstTransport, initial), "initial candidate update rejected");
    check(engine->remoteCandidates.size() == 1, "initial candidate not delivered exactly once");
    check(engine->peerUser == QLatin1String("peer")
              && engine->peerPass == QLatin1String("peer-password-for-ice-testing"),
          "candidate update erased ICE credentials");
    check(deliver(secondTransport, initial), "BUNDLE follower update rejected");
    check(engine->remoteCandidates.size() == 1, "BUNDLE follower replay reached ICE");

    QDomDocument mixedDoc;
    const auto   mixed = update(mixedDoc, ns, true);
    check(deliver(secondTransport, mixed), "mixed candidate update rejected");
    check(engine->remoteCandidates.size() == 2, "new candidate lost or known candidate replayed");
    check(engine->remoteCandidates[0]->id == QLatin1String("first")
              && engine->remoteCandidates[1]->id == QLatin1String("second"),
          "candidate order changed");
    check(!engine->checkList.pairs.isEmpty(), "new candidates did not produce pairs");

    // Nomination empties the checklist. Replaying a follower's snapshot must
    // not repopulate it or wake ordinary checks on an active association.
    engine->state = Ice176::Private::Active;
    engine->checkList.pairs.clear();
    engine->checkTimer.stop();
    engine->canStartChecks = true;
    for (int i = 0; i < 14; ++i)
        check(deliver(secondTransport, mixed), "active snapshot rejected");
    check(engine->remoteCandidates.size() == 2, "active snapshot grew the remote candidate list");
    check(engine->checkList.pairs.isEmpty() && !engine->checkTimer.isActive(), "replay restarted ICE checks");
    delete first;
    check(deliver(secondTransport, mixed) && engine->remoteCandidates.size() == 2,
          "first content removal reset association replay tracking");
    delete second;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    // Separate sessions may use the same candidate IDs, in either ICE profile.
    for (const auto &ns : { J::ICE::NS, J::ICE::NS_ICE_UDP }) {
        exercise(ns, true);
        exercise(ns, false);
    }
    qInfo("ICE association candidate replay regressions passed");
}
