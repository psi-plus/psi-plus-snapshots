// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main
#define private public
#include <iris/jingle-rtp.h>
#undef private
#include <iris/xmpp.h>
#include <iris/xmpp_clientstream.h>

namespace R        = XMPP::Jingle::RTP;
using PolicyStatus = R::DirectionController::Status;

class PolicyApplication : public R::Application {
public:
    explicit PolicyApplication(const QSharedPointer<R::Pad> &pad) :
        R::Application(pad, "audio", J::Origin::Initiator, J::Origin::Both)
    {
        _transport = QSharedPointer<TestTransport>::create();
        setState(J::State::Active);
    }
};

static void flush()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

static J::OutgoingUpdate consume(PolicyApplication &content)
{
    check(content.evaluateOutgoingUpdate().action == J::Action::ContentModify, "policy did not schedule direction");
    return content.takeOutgoingUpdate();
}

static void crossedPolicies(bool flushBeforeError)
{
    class NoNetworkConnector : public Connector {
    public:
        void        setOptHostPort(const QString &, quint16) override { }
        void        connectToServer(const QString &) override { }
        ByteStream *stream() const override { return nullptr; }
        void        done() override { }
    } connector;
    ClientStream stream(&connector);
    Client       client;
    client.connectToServer(&stream, Jid("test@example.test/device"));
    Result success(client.rootTask(), true);
    class TieFailure : public Task {
    public:
        explicit TieFailure(Task *parent) : Task(parent)
        {
            QDomDocument  doc;
            auto          iq = doc.createElement("iq");
            Stanza::Error error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::Conflict);
            J::ErrorUtil::fill(doc, error, J::ErrorUtil::TieBreak);
            iq.appendChild(error.toXml(doc, "jabber:client"));
            setError(iq);
        }
    } failure(client.rootTask());
    J::Session        initiator(client.jingleManager(), Jid("r@example.test/device"), J::Origin::Initiator);
    J::Session        responder(client.jingleManager(), Jid("i@example.test/device"), J::Origin::Responder);
    auto              ip = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &initiator,
                                                          std::shared_ptr<R::MediaProvider>(), QStringList());
    auto              rp = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &responder,
                                                          std::shared_ptr<R::MediaProvider>(), QStringList());
    PolicyApplication ia(ip), ra(rp);
    ia.incomingContentModify(J::Origin::None);
    ra.incomingContentModify(J::Origin::None);
    ip->directionController()->setLocalSending(&ia, true);
    rp->directionController()->setLocalSending(&ra, true);
    flush();
    auto       iu = consume(ia), ru = consume(ra);
    const auto itx = startContentModify(initiator, { &iu });
    const auto rtx = startContentModify(responder, { &ru });
    check(initiator.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(ru)).solution
              == J::TieBreaker::Solution::Break,
          "initiator did not win policy collision");
    const auto resolution = responder.tieBreaker()->resolveIncoming(J::Action::ContentModify, payload(iu));
    check(resolution.solution == J::TieBreaker::Solution::Postpone, "responder did not postpone policy collision");
    ra.incomingContentModify(J::Origin::Initiator);
    responder.tieBreaker()->incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
    if (flushBeforeError)
        flush(); // controller may already have queued the fresh target while old IQ waits
    finishContentModify(initiator, itx, true, &success, { &iu });
    finishContentModify(responder, rtx, false, &failure, { &ru });
    flush();
    auto merged = consume(ra);
    check(firstContent(merged).attribute("senders") == "both",
          "tie-break recovery replayed obsolete responder-only target");
    ia.incomingContentModify(J::Origin::Both);
    acknowledge(merged, &success);
    flush();
    check(ia.senders() == J::Origin::Both && ra.senders() == J::Origin::Both,
          "crossed local-send policies failed to converge");
    check(ip->directionController()->policy(&ia)->status == PolicyStatus::Satisfied
              && rp->directionController()->policy(&ra)->status == PolicyStatus::Satisfied,
          "converged policy remained pending");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    Result           success(client.rootTask(), true), failure(client.rootTask(), false);
    for (auto role : { J::Origin::Initiator, J::Origin::Responder }) {
        J::Session        session(client.jingleManager(), Jid("peer@example.test/device"), role);
        auto              pad        = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                                      std::shared_ptr<R::MediaProvider>(), QStringList());
        auto              controller = pad->directionController();
        PolicyApplication content(pad);
        const auto        peer = role == J::Origin::Initiator ? J::Origin::Responder : J::Origin::Initiator;

        controller->setLocalSending(&content, true);
        flush();
        check(controller->policy(&content)->status == PolicyStatus::Satisfied,
              "already satisfied local policy not observed");
        auto device     = controller->suspendLocalSending(&content);
        auto permission = controller->suspendLocalSending(&content);
        check(!content.allowsRtp(true) && content.allowsRtp(false),
              "constraint did not immediately close only send gate");
        flush();
        auto mute = consume(content);
        acknowledge(mute, &success);
        flush();
        check(content.senders() == peer && controller->policy(&content)->status == PolicyStatus::Blocked,
              "local constraint overwrote peer sending or falsely satisfied blocked desire");
        device.reset();
        flush();
        check(!content.allowsRtp(true), "removing one constraint overrode another");
        permission.reset();
        flush();
        auto restore = consume(content);
        acknowledge(restore, &success);
        flush();
        check(content.senders() == J::Origin::Both && content.allowsRtp(true),
              "device recovery did not restore allowed desire");

        // New preference must survive an old accepted mute already on the wire.
        controller->setLocalSending(&content, false);
        flush();
        auto oldMute = consume(content);
        controller->setLocalSending(&content, true);
        flush();
        acknowledge(oldMute, &success);
        flush();
        auto newSend = consume(content);
        acknowledge(newSend, &success);
        flush();
        check(content.senders() == J::Origin::Both, "stale mute ACK replaced newer desired sending");

        // Reconciliation changes only our bit, not the peer's live direction.
        content.incomingContentModify(peer);
        flush();
        auto merge = consume(content);
        check(firstContent(merge).attribute("senders") == "both", "local-send recovery replayed an exact old mask");
        acknowledge(merge, &success);
        flush();

        // Generic rejection is visible and bounded; no automatic hot retry.
        controller->setLocalSending(&content, false);
        flush();
        auto rejected = consume(content);
        acknowledge(rejected, &failure);
        flush();
        check(controller->policy(&content)->status == PolicyStatus::Failed && !content.allowsRtp(true),
              "failed policy reopened sending or remained indefinitely pending");
        check(content.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "generic failure entered an automatic retry loop");

        // Dropping a hardware restriction cannot resurrect revoked consent.
        auto hardware = controller->suspendLocalSending(&content);
        controller->setLocalSending(&content, false);
        hardware.reset();
        check(!content.allowsRtp(true), "constraint removal resurrected revoked sending consent");
    }

    // Tokens identify an Application incarnation, never merely (creator,name).
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       pad        = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                               std::shared_ptr<R::MediaProvider>(), QStringList());
        auto       controller = pad->directionController();
        auto       old        = std::make_unique<PolicyApplication>(pad);
        auto       stale      = controller->suspendLocalSending(old.get());
        old.reset();
        PolicyApplication replacement(pad);
        auto              current = controller->suspendLocalSending(&replacement);
        controller->setLocalSending(&replacement, true);
        stale.reset();
        check(!replacement.allowsRtp(true), "stale token released replacement content's constraint");
        controller->setLocalSending(&replacement, false);
        current.reset();
        flush();
        check(!replacement.allowsRtp(true), "unblocked replacement inferred consent from negotiated senders");
    }
    crossedPolicies(false);
    crossedPolicies(true);

    // Successful IQs do not justify an endless reassertion fight with the peer.
    {
        J::Session        session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto              pad = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                               std::shared_ptr<R::MediaProvider>(), QStringList());
        PolicyApplication content(pad);
        auto              controller = pad->directionController();
        controller->setLocalSending(&content, true);
        flush();
        for (int i = 0; i < 3; ++i) {
            content.incomingContentModify(J::Origin::Responder);
            flush();
            acknowledge(consume(content), &success);
            flush();
        }
        content.incomingContentModify(J::Origin::Responder);
        flush();
        check(controller->policy(&content)->status == PolicyStatus::Failed && !content.allowsRtp(true),
              "opposing peer policy caused unbounded reassertion");
        check(content.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "reconciliation limit left a stale proposal");
    }

    // Policy changes from a synchronous scheduling notification supersede the
    // target being scheduled. The old call must not overwrite the new revision.
    {
        J::Session        session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto              pad = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                               std::shared_ptr<R::MediaProvider>(), QStringList());
        PolicyApplication content(pad);
        auto              controller = pad->directionController();
        bool              changed    = false;
        QObject::connect(&content, &J::Application::updated, &app, [&] {
            if (!changed) {
                changed = true;
                controller->setLocalSending(&content, true);
            }
        });
        controller->setLocalSending(&content, false);
        flush();
        check(changed && controller->policy(&content)->desiredSending && content.allowsRtp(true),
              "reentrant policy change was overwritten by old scheduling call");
        check(content.evaluateOutgoingUpdate().action == J::Action::NoAction, "superseded scheduling kept old mute");
    }
    // Destroy the whole Pad/controller from Application::updated while a
    // queued reconciliation is publishing its request.
    {
        J::Session session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);
        auto       pad     = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                            std::shared_ptr<R::MediaProvider>(), QStringList());
        auto       content = std::make_unique<PolicyApplication>(pad);
        QPointer<R::DirectionController> controller(pad->directionController());
        auto                             token = controller->suspendLocalSending(content.get());
        QObject::connect(content.get(), &J::Application::updated, &app, [&] {
            content.reset();
            pad.reset();
        });
        flush();
        check(!controller && !content, "reconciliation deletion fixture did not run");
        token.reset(); // stale constraint must not call a deleted coordinator
    }
    return 0;
}
