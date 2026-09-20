// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main
#include <QElapsedTimer>
#include <QThread>
#include <iris/jingle-rtp.h>

namespace R = XMPP::Jingle::RTP;
using Op    = R::DirectionOperation;

class OperationApplication : public R::Application {
public:
    OperationApplication(const QSharedPointer<R::Pad> &pad, const QString &name) :
        R::Application(pad, name, J::Origin::Initiator, J::Origin::Both)
    {
        _transport = QSharedPointer<TestTransport>::create();
        setState(J::State::Active);
    }
};

static void flush()
{
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
}
static J::OutgoingUpdate consume(OperationApplication &content)
{
    check(content.evaluateOutgoingUpdate().action == J::Action::ContentModify, "operation did not schedule attempt");
    return content.takeOutgoingUpdate();
}

struct Fixture {
    Client                  client;
    J::Session              session { client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator };
    QSharedPointer<R::Pad>  pad = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), &session,
                                                                 std::shared_ptr<R::MediaProvider>(), QStringList());
    OperationApplication    audio { pad, "audio" }, video { pad, "video" };
    R::DirectionController *controller = pad->directionController();
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, true } });
        int     finished  = 0;
        QObject::connect(operation.get(), &Op::finished, &app, [&] { ++finished; });
        check(operation->state() == Op::State::Pending && !finished, "no-op completed inline");
        flush();
        check(operation->state() == Op::State::Succeeded && finished == 1, "no-op did not complete once");
        f.audio.incomingContentModify(J::Origin::Responder);
        operation->cancel();
        flush();
        check(operation->state() == Op::State::Succeeded && finished == 1, "terminal operation reopened");
    }
    {
        Fixture f, foreign;
        for (const QList<R::DirectionController::Request> requests :
             { QList<R::DirectionController::Request> { { &f.audio, false }, { &foreign.video, false } },
               QList<R::DirectionController::Request> { { &f.audio, false }, { &f.audio, true } },
               QList<R::DirectionController::Request> { { &f.audio, false }, { nullptr, false } } }) {
            auto operation = f.controller->requestLocalSending(requests);
            check(!f.controller->policy(&f.audio), "invalid batch partially installed policy");
            flush();
            check(operation->state() == Op::State::Failed && operation->error() == Op::Error::InvalidRequest,
                  "invalid batch did not produce an explicit failure");
        }
        auto zeroDeadline = f.controller->requestLocalSending({ { &f.audio, false } }, 0);
        flush();
        check(zeroDeadline->error() == Op::Error::InvalidRequest && !f.controller->policy(&f.audio),
              "invalid deadline changed policy");
    }
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, true }, { &f.video, false } });
        flush();
        auto items = operation->items();
        check(operation->state() == Op::State::Pending && items[0].state == Op::ItemState::Satisfied
                  && items[1].state == Op::ItemState::Pending,
              "batch did not expose per-content progress");
        auto   update = consume(f.video);
        Result success(f.client.rootTask(), true);
        acknowledge(update, &success);
        flush();
        check(operation->state() == Op::State::Succeeded, "completed batch did not finish");
    }
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, false }, { &f.video, false } });
        flush();
        auto audioUpdate = consume(f.audio);
        auto replacement = f.controller->requestLocalSending({ { &f.audio, true } });
        flush();
        check(operation->state() == Op::State::Superseded, "overlapping revision did not supersede old operation");
        check(!f.controller->policy(&f.video)->desiredSending, "supersession rolled back unaffected sibling policy");
        Result success(f.client.rootTask(), true);
        acknowledge(audioUpdate, &success); // old mute is already on the wire
        flush();
        acknowledge(consume(f.audio), &success);
        flush();
        check(replacement->state() == Op::State::Succeeded && operation->state() == Op::State::Superseded,
              "stale ACK reopened old operation or displaced replacement");
    }
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, false } });
        flush();
        auto update = consume(f.audio);
        operation->cancel();
        flush();
        check(operation->state() == Op::State::Cancelled && !f.controller->policy(&f.audio)->desiredSending,
              "cancelling observation reverted mute policy");
        Result success(f.client.rootTask(), true);
        acknowledge(update, &success);
        flush();
        check(operation->state() == Op::State::Cancelled && f.audio.senders() == J::Origin::Responder,
              "cancellation tried to unsend an IQ or reopened observation");
        auto detached = f.controller->requestLocalSending({ { &f.video, false } });
        detached.reset();
        flush();
        check(!f.controller->policy(&f.video)->desiredSending, "observer destruction reverted policy");
    }
    {
        Fixture f;
        auto    constraint = f.controller->suspendLocalSending(&f.audio, "microphone unavailable");
        auto    operation  = f.controller->requestLocalSending({ { &f.audio, true } });
        flush();
        check(operation->items()[0].state == Op::ItemState::Blocked
                  && operation->items()[0].blockedReasons.contains("microphone unavailable"),
              "blocked operation lost constraint reason");
        Result success(f.client.rootTask(), true);
        acknowledge(consume(f.audio), &success);
        flush();
        check(operation->state() == Op::State::Pending, "receive-only effective target falsely satisfied send desire");
        constraint.reset();
        flush();
        acknowledge(consume(f.audio), &success);
        flush();
        check(operation->state() == Op::State::Succeeded, "unblocked desire did not complete");
    }
    {
        Fixture       f;
        auto          constraint = f.controller->suspendLocalSending(&f.audio, "permission denied");
        auto          operation  = f.controller->requestLocalSending({ { &f.audio, true } }, 20);
        QElapsedTimer timer;
        timer.start();
        while (operation->state() == Op::State::Pending && timer.elapsed() < 2000) {
            flush();
            QThread::msleep(1);
        }
        check(operation->state() == Op::State::Failed && operation->error() == Op::Error::Timeout,
              "blocked observation had no finite deadline");
        check(f.controller->policy(&f.audio)->desiredSending, "observer timeout destroyed durable preference");
        constraint.reset();
        flush();
        check(operation->state() == Op::State::Failed, "timed-out operation reopened after device return");
    }
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, false } });
        flush();
        Result failure(f.client.rootTask(), false);
        acknowledge(consume(f.audio), &failure);
        flush();
        auto item = operation->items()[0];
        check(operation->state() == Op::State::Failed && item.error == Op::Error::PolicyFailure
                  && item.failure == R::DirectionController::Failure::Signaling && item.signalingError,
              "operation lost signaling failure details");
    }
    {
        Fixture                          f;
        std::vector<std::unique_ptr<Op>> pending;
        for (int i = 0; i < 32; ++i)
            pending.push_back(f.controller->requestLocalSending({ { &f.audio, true } }));
        const auto revision = f.controller->policy(&f.audio)->revision;
        auto       excess   = f.controller->requestLocalSending({ { &f.audio, false } });
        check(f.controller->policy(&f.audio)->revision == revision, "capacity rejection mutated policy");
        flush();
        check(excess->state() == Op::State::Failed && excess->error() == Op::Error::Capacity,
              "pending observer count was not bounded");
        auto next = f.controller->requestLocalSending({ { &f.audio, true } });
        flush();
        check(next->state() == Op::State::Succeeded, "terminal observers retained capacity slots");
    }
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, true } });
        QObject::connect(operation.get(), &Op::progressChanged, &app, [&] { operation.reset(); });
        flush();
        check(!operation, "progress callback deletion fixture did not run");
    }
    {
        Fixture f;
        auto    oldContent   = std::make_unique<OperationApplication>(f.pad, "recreated");
        auto    oldOperation = f.controller->requestLocalSending({ { oldContent.get(), false } });
        flush();
        oldContent.reset();
        auto newContent   = std::make_unique<OperationApplication>(f.pad, "recreated");
        auto newOperation = f.controller->requestLocalSending({ { newContent.get(), true } });
        flush();
        check(oldOperation->state() == Op::State::Failed && oldOperation->error() == Op::Error::ContentGone
                  && newOperation->state() == Op::State::Succeeded,
              "old operation transferred to a new content incarnation");
    }
    {
        Fixture f;
        auto    operation = f.controller->requestLocalSending({ { &f.audio, false } });
        int     finished  = 0;
        QObject::connect(operation.get(), &Op::progressChanged, &app, [&] { operation->cancel(); });
        QObject::connect(operation.get(), &Op::finished, &app, [&] {
            ++finished;
            operation.reset();
        });
        flush();
        check(!operation && finished == 1 && !f.controller->policy(&f.audio)->desiredSending,
              "reentrant cancel/completion deletion did not finish once or changed policy");
    }
    {
        Client client;
        auto   session = std::make_unique<J::Session>(client.jingleManager(), Jid("peer@example.test/device"),
                                                      J::Origin::Initiator);
        auto   pad     = QSharedPointer<R::Pad>::create(client.jingleManager()->rtpManager(), session.get(),
                                                        std::shared_ptr<R::MediaProvider>(), QStringList());
        OperationApplication content(pad, "surviving");
        auto                 operation = pad->directionController()->requestLocalSending({ { &content, false } });
        flush();
        session.reset(); // Pad/content can be externally retained; no application state signal follows.
        flush();
        check(operation->state() == Op::State::Failed && operation->error() == Op::Error::ContentGone,
              "session destruction left surviving operation pending until deadline");
    }
    {
        std::unique_ptr<Op> survivor;
        {
            Fixture f;
            survivor = f.controller->requestLocalSending({ { &f.audio, false } });
        }
        flush();
        check(survivor->state() == Op::State::Failed && survivor->error() == Op::Error::ContentGone,
              "operation outlived controller without terminal notification");
    }
    return 0;
}
