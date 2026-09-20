// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main

#define private public
#include <iris/jingle-rtp.h>
#undef private

namespace R = XMPP::Jingle::RTP;

struct WireContent {
    QString creator;
    QString name;
    QString senders;
};

static WireContent wireContent(const J::OutgoingUpdate &update)
{
    const auto content = firstContent(update);
    return { content.attribute(QStringLiteral("creator")), content.attribute(QStringLiteral("name")),
             content.attribute(QStringLiteral("senders")) };
}

static QDomElement contentModifyIq(Client &receiver, const Jid &from, const QString &sid, const QString &id,
                                   const QList<WireContent> &contents)
{
    auto *doc = receiver.doc();
    auto  iq  = doc->createElementNS(QStringLiteral("jabber:client"), QStringLiteral("iq"));
    iq.setAttribute(QStringLiteral("type"), QStringLiteral("set"));
    iq.setAttribute(QStringLiteral("from"), from.full());
    iq.setAttribute(QStringLiteral("id"), id);

    auto jingle = doc->createElementNS(J::NS, QStringLiteral("jingle"));
    jingle.setAttribute(QStringLiteral("action"), QStringLiteral("content-modify"));
    jingle.setAttribute(QStringLiteral("sid"), sid);
    for (const auto &wire : contents) {
        auto content = doc->createElementNS(J::NS, QStringLiteral("content"));
        content.setAttribute(QStringLiteral("creator"), wire.creator);
        content.setAttribute(QStringLiteral("name"), wire.name);
        content.setAttribute(QStringLiteral("senders"), wire.senders);
        jingle.appendChild(content);
    }
    iq.appendChild(jingle);
    return iq;
}

static void dispatchModify(Client &receiver, const Jid &from, const QString &sid, const QString &id,
                           const QList<WireContent> &contents)
{
    const auto iq = contentModifyIq(receiver, from, sid, id, contents);
    check(receiver.rootTask()->take(iq), "JTPush did not consume content-modify IQ");
}

static void completeCrossed(J::Session &initiator, J::Session &responder, quint64 initiatorTransaction,
                            quint64 responderTransaction, const J::OutgoingUpdate &initiatorUpdate,
                            const J::OutgoingUpdate &responderUpdate, Task *success, Task *failure,
                            bool responderResultFirst)
{
    if (responderResultFirst) {
        finishContentModify(responder, responderTransaction, false, failure, { &responderUpdate });
        finishContentModify(initiator, initiatorTransaction, true, success, { &initiatorUpdate });
    } else {
        finishContentModify(initiator, initiatorTransaction, true, success, { &initiatorUpdate });
        finishContentModify(responder, responderTransaction, false, failure, { &responderUpdate });
    }
}

static void checkInitiatorPacketGates(Client &client, J::Session &session, J::Origin senders, bool sending,
                                      bool receiving)
{
    auto           pad = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                        std::shared_ptr<R::MediaProvider>(), QStringList());
    R::Application gate(pad, QStringLiteral("gate"), J::Origin::Responder, senders);
    check(gate.allowsRtp(true) == sending, "negotiated senders did not reach the RTP sending gate");
    check(gate.allowsRtp(false) == receiving, "negotiated senders did not reach the RTP receiving gate");
}

static void crossedThroughDispatcher(J::Origin initiatorTarget, J::Origin responderTarget, bool responderIqFirst,
                                     bool responderResultFirst, bool verifyPacketGates)
{
    Client    initiatorClient;
    Client    responderClient;
    const Jid initiatorJid(QStringLiteral("initiator@example.test/device"));
    const Jid responderJid(QStringLiteral("responder@example.test/device"));

    J::Session initiator(initiatorClient.jingleManager(), responderJid, J::Origin::Initiator);
    J::Session responder(responderClient.jingleManager(), initiatorJid, J::Origin::Responder);
    const auto initiatorSid = initiatorClient.jingleManager()->registerSession(&initiator);
    const auto responderSid = responderClient.jingleManager()->registerSession(&responder);

    // Responder-created content gives the responder-side Session the exact wire
    // (creator,name) key of the initiator winner. The initiator-side duplicate is
    // intentionally rejected by JTPush before content lookup.
    auto initiatorApp = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    auto responderApp = new TestApplication(&responder, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    initiator.addContent(initiatorApp);
    responder.addContent(responderApp);
    initiatorApp->activate();
    responderApp->activate();

    int initiatorPeerUpdates = 0;
    int responderPeerUpdates = 0;
    QObject::connect(initiatorApp, &J::Application::sendersChangedByPeer, initiatorApp,
                     [&](J::Origin) { ++initiatorPeerUpdates; });
    QObject::connect(responderApp, &J::Application::sendersChangedByPeer, responderApp,
                     [&](J::Origin) { ++responderPeerUpdates; });

    check(initiatorApp->requestSenders(initiatorTarget), "initiator dispatcher request rejected");
    check(responderApp->requestSenders(responderTarget), "responder dispatcher request rejected");
    check(initiatorApp->evaluateOutgoingUpdate().action == J::Action::ContentModify,
          "initiator dispatcher request was not evaluated");
    check(responderApp->evaluateOutgoingUpdate().action == J::Action::ContentModify,
          "responder dispatcher request was not evaluated");
    auto       initiatorUpdate      = initiatorApp->takeOutgoingUpdate();
    auto       responderUpdate      = responderApp->takeOutgoingUpdate();
    const auto initiatorTransaction = startContentModify(initiator, { &initiatorUpdate });
    const auto responderTransaction = startContentModify(responder, { &responderUpdate });

    const auto initiatorWire              = wireContent(initiatorUpdate);
    const auto responderWire              = wireContent(responderUpdate);
    auto       dispatchResponderDuplicate = [&]() {
        dispatchModify(initiatorClient, responderJid, initiatorSid, QStringLiteral("responder-duplicate"),
                             { responderWire });
        check(!initiator.lastError().has_value(),
                    "initiator parsed the losing duplicate instead of taking the JTPush tie-break path");
        check(initiatorApp->senders() == J::Origin::Both && initiatorPeerUpdates == 0,
                    "losing responder content-modify mutated initiator state");
    };
    auto dispatchInitiatorWinner = [&]() {
        dispatchModify(responderClient, initiatorJid, responderSid, QStringLiteral("initiator-winner"),
                       { initiatorWire });
        check(responderApp->senders() == initiatorTarget && responderPeerUpdates == 1,
              "responder did not apply the initiator content-modify winner");
    };

    if (responderIqFirst) {
        dispatchResponderDuplicate();
        dispatchInitiatorWinner();
    } else {
        dispatchInitiatorWinner();
        dispatchResponderDuplicate();
    }

    Result success(initiatorClient.rootTask(), true);
    Result failure(initiatorClient.rootTask(), false);
    completeCrossed(initiator, responder, initiatorTransaction, responderTransaction, initiatorUpdate, responderUpdate,
                    &success, &failure, responderResultFirst);

    check(initiatorApp->senders() == initiatorTarget && responderApp->senders() == initiatorTarget,
          "dispatcher-level crossed content-modify did not converge to initiator state");
    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(responderUpdate)).solution
                  == J::TieBreaker::Solution::Continue
              && responder.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(initiatorUpdate)).solution
                  == J::TieBreaker::Solution::Continue,
          "dispatcher-level crossed content-modify left collision state active");

    if (verifyPacketGates) {
        check(initiatorTarget == J::Origin::Initiator, "packet gate fixture expects initiator-only direction");
        checkInitiatorPacketGates(initiatorClient, initiator, initiatorTarget, true, false);
        checkInitiatorPacketGates(responderClient, responder, initiatorTarget, false, true);
    }
}

static void multiContentThroughDispatcher()
{
    Client    initiatorClient;
    Client    responderClient;
    const Jid initiatorJid(QStringLiteral("multi-initiator@example.test/device"));
    const Jid responderJid(QStringLiteral("multi-responder@example.test/device"));

    J::Session initiator(initiatorClient.jingleManager(), responderJid, J::Origin::Initiator);
    J::Session responder(responderClient.jingleManager(), initiatorJid, J::Origin::Responder);
    const auto initiatorSid = initiatorClient.jingleManager()->registerSession(&initiator);
    const auto responderSid = responderClient.jingleManager()->registerSession(&responder);

    auto ia = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    auto iv = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("video"), J::Origin::Responder);
    auto ra = new TestApplication(&responder, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    auto rv = new TestApplication(&responder, J::Origin::Both, QStringLiteral("video"), J::Origin::Responder);
    for (auto *content : { ia, iv }) {
        initiator.addContent(content);
        content->activate();
    }
    for (auto *content : { ra, rv }) {
        responder.addContent(content);
        content->activate();
    }

    check(ia->requestSenders(J::Origin::Initiator) && iv->requestSenders(J::Origin::None),
          "initiator multi-content requests rejected");
    check(ra->requestSenders(J::Origin::Responder) && rv->requestSenders(J::Origin::Initiator),
          "responder multi-content requests rejected");
    check(ia->evaluateOutgoingUpdate().action == J::Action::ContentModify
              && iv->evaluateOutgoingUpdate().action == J::Action::ContentModify
              && ra->evaluateOutgoingUpdate().action == J::Action::ContentModify
              && rv->evaluateOutgoingUpdate().action == J::Action::ContentModify,
          "multi-content request was not evaluated as content-modify");

    auto       iau                  = ia->takeOutgoingUpdate();
    auto       ivu                  = iv->takeOutgoingUpdate();
    auto       rau                  = ra->takeOutgoingUpdate();
    auto       rvu                  = rv->takeOutgoingUpdate();
    const auto initiatorTransaction = startContentModify(initiator, { &iau, &ivu });
    const auto responderTransaction = startContentModify(responder, { &rau, &rvu });

    dispatchModify(initiatorClient, responderJid, initiatorSid, QStringLiteral("multi-responder-duplicate"),
                   { wireContent(rau), wireContent(rvu) });
    check(!initiator.lastError().has_value() && ia->senders() == J::Origin::Both && iv->senders() == J::Origin::Both,
          "multi-content losing action escaped the dispatcher tie-break");

    dispatchModify(responderClient, initiatorJid, responderSid, QStringLiteral("multi-initiator-winner"),
                   { wireContent(iau), wireContent(ivu) });
    check(ra->senders() == J::Origin::Initiator && rv->senders() == J::Origin::None,
          "multi-content initiator winner was not applied atomically");

    Result success(initiatorClient.rootTask(), true);
    Result failure(initiatorClient.rootTask(), false);
    finishContentModify(responder, responderTransaction, false, &failure, { &rau, &rvu });
    finishContentModify(initiator, initiatorTransaction, true, &success, { &iau, &ivu });

    check(ia->senders() == J::Origin::Initiator && ra->senders() == J::Origin::Initiator,
          "multi-content audio did not converge");
    check(iv->senders() == J::Origin::None && rv->senders() == J::Origin::None, "multi-content video did not converge");
}

static void deletionWhileIqPending()
{
    Client    initiatorClient;
    Client    responderClient;
    const Jid initiatorJid(QStringLiteral("delete-initiator@example.test/device"));
    const Jid responderJid(QStringLiteral("delete-responder@example.test/device"));

    J::Session initiator(initiatorClient.jingleManager(), responderJid, J::Origin::Initiator);
    J::Session responder(responderClient.jingleManager(), initiatorJid, J::Origin::Responder);
    const auto initiatorSid = initiatorClient.jingleManager()->registerSession(&initiator);
    const auto responderSid = responderClient.jingleManager()->registerSession(&responder);

    auto ia = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    auto iv = new TestApplication(&initiator, J::Origin::Both, QStringLiteral("video"), J::Origin::Responder);
    auto ra = new TestApplication(&responder, J::Origin::Both, QStringLiteral("audio"), J::Origin::Responder);
    auto rv = new TestApplication(&responder, J::Origin::Both, QStringLiteral("video"), J::Origin::Responder);
    for (auto *content : { ia, iv }) {
        initiator.addContent(content);
        content->activate();
    }
    for (auto *content : { ra, rv }) {
        responder.addContent(content);
        content->activate();
    }

    check(ia->requestSenders(J::Origin::Initiator) && iv->requestSenders(J::Origin::Responder),
          "initiator deletion fixture requests rejected");
    check(ra->requestSenders(J::Origin::Responder) && rv->requestSenders(J::Origin::Initiator),
          "responder deletion fixture requests rejected");
    ia->evaluateOutgoingUpdate();
    iv->evaluateOutgoingUpdate();
    ra->evaluateOutgoingUpdate();
    rv->evaluateOutgoingUpdate();
    auto                     iau = ia->takeOutgoingUpdate();
    auto                     ivu = iv->takeOutgoingUpdate();
    auto                     rau = ra->takeOutgoingUpdate();
    auto                     rvu = rv->takeOutgoingUpdate();
    const QList<WireContent> initiatorWire { wireContent(iau), wireContent(ivu) };
    const QList<WireContent> responderWire { wireContent(rau), wireContent(rvu) };
    const auto               initiatorTransaction = startContentModify(initiator, { &iau, &ivu });
    const auto               responderTransaction = startContentModify(responder, { &rau, &rvu });

    // Session::doStep stores callbacks behind QPointer<Application>. Clearing the
    // callback here models that skip after the Application disappears while the IQ
    // itself remains outstanding.
    std::get<1>(ivu) = {};
    delete iv;
    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(rau)).solution
              == J::TieBreaker::Solution::Break,
          "content deletion incorrectly ended the Session-level IQ collision");

    dispatchModify(initiatorClient, responderJid, initiatorSid, QStringLiteral("deleted-sibling-duplicate"),
                   responderWire);
    check(!initiator.lastError().has_value() && ia->senders() == J::Origin::Both,
          "deleted sibling made the losing batch bypass tie-break");

    dispatchModify(responderClient, initiatorJid, responderSid, QStringLiteral("deleted-sibling-winner"),
                   initiatorWire);
    check(ra->senders() == J::Origin::Initiator && rv->senders() == J::Origin::Responder,
          "peer did not apply the already-sent initiator batch after local content deletion");

    Result success(initiatorClient.rootTask(), true);
    Result failure(initiatorClient.rootTask(), false);
    finishContentModify(responder, responderTransaction, false, &failure, { &rau, &rvu });
    finishContentModify(initiator, initiatorTransaction, true, &success, { &iau });

    check(ia->senders() == J::Origin::Initiator && ra->senders() == J::Origin::Initiator,
          "surviving content did not converge after sibling deletion");
    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(rau)).solution
              == J::TieBreaker::Solution::Continue,
          "completed IQ kept collision active after sibling deletion");
}

static void postIqActionIsNotTieBroken()
{
    Client     client;
    const Jid  peer(QStringLiteral("later-peer@example.test/device"));
    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    const auto sid = client.jingleManager()->registerSession(&session);

    auto content = new TestApplication(&session, J::Origin::Both, QStringLiteral("audio"), J::Origin::Initiator);
    session.addContent(content);
    content->activate();
    check(content->requestSenders(J::Origin::Responder), "post-IQ local request rejected");
    content->evaluateOutgoingUpdate();
    auto       update      = content->takeOutgoingUpdate();
    const auto transaction = startContentModify(session, { &update });

    bool dispatched  = false;
    int  peerUpdates = 0;
    QObject::connect(content, &J::Application::sendersChangedByPeer, content, [&](J::Origin) { ++peerUpdates; });
    QObject::connect(content, &J::Application::sendersChanged, content, [&](J::Origin senders) {
        if (senders != J::Origin::Responder || dispatched)
            return;
        dispatched = true;
        dispatchModify(client, peer, sid, QStringLiteral("later-content-modify"),
                       { { QStringLiteral("initiator"), QStringLiteral("audio"), QStringLiteral("none") } });
    });

    Result success(client.rootTask(), true);
    // This ordering is the production JT::finished ordering: the Jingle IQ stops
    // being collision-active before Application ACK callbacks are invoked.
    session.tieBreaker()->outgoingFinished(transaction, std::nullopt);
    acknowledge(update, &success);
    session.tieBreaker()->outgoingCallbacksFinished(transaction);

    check(dispatched, "post-IQ reentrant peer action was not dispatched");
    check(!session.lastError().has_value() && peerUpdates == 1,
          "post-IQ peer action was rejected instead of being applied");
    check(content->senders() == J::Origin::None,
          "post-IQ peer content-modify did not replace the completed local direction");
}

static void deletingSessionFromResolver()
{
    Client     client;
    const Jid  peer(QStringLiteral("deletion@example.test/device"));
    auto       session = std::make_unique<J::Session>(client.jingleManager(), peer, J::Origin::Initiator);
    const auto sid     = client.jingleManager()->registerSession(session.get());
    class Deleter : public J::TieBreaker::Resolver {
    public:
        std::function<void()>   destroy;
        J::TieBreaker::Solution resolve(const QDomElement &, const QDomElement &) override
        {
            destroy();
            return J::TieBreaker::Solution::Continue;
        }
    } resolver;
    resolver.destroy  = [&] { session.reset(); };
    auto registration = session->tieBreaker()->registerResolver(J::Action::ContentModify, &resolver);
    session->tieBreaker()->outgoingStarted(J::Action::ContentModify, client.doc()->createElement("jingle"));
    dispatchModify(client, peer, sid, QStringLiteral("delete-from-resolve"),
                   { { QStringLiteral("initiator"), QStringLiteral("audio"), QStringLiteral("none") } });
    check(!session, "resolver did not delete the Session");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    // Different and identical intents, both incoming IQ orderings and both local
    // IQ-result orderings must all converge to the initiator's action.
    for (const bool responderIqFirst : { false, true }) {
        for (const bool responderResultFirst : { false, true }) {
            crossedThroughDispatcher(J::Origin::Initiator, J::Origin::Responder, responderIqFirst, responderResultFirst,
                                     !responderIqFirst && !responderResultFirst);
            crossedThroughDispatcher(J::Origin::Responder, J::Origin::Responder, responderIqFirst, responderResultFirst,
                                     false);
        }
    }

    multiContentThroughDispatcher();
    deletionWhileIqPending();
    postIqActionIsNotTieBroken();
    deletingSessionFromResolver();

    qInfo("Dispatcher-level crossed content-modify regressions passed");
    return 0;
}
