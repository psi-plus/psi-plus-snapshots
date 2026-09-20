// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main

using Attempt = J::Application::SendersAttemptResult;

static J::OutgoingUpdate consume(TestApplication &content)
{
    check(content.evaluateOutgoingUpdate().action == J::Action::ContentModify, "missing direction attempt");
    return content.takeOutgoingUpdate();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    Result           success(client.rootTask(), true), failure(client.rootTask(), false);
    J::Session       session(client.jingleManager(), Jid("peer@example.test/device"), J::Origin::Initiator);

    // A failed attempt belongs to its revision, not to the newest request. A
    // stale/duplicate callback must never complete a newer in-flight attempt.
    {
        TestApplication content(&session);
        content.activate();
        QList<Attempt> results;
        QObject::connect(&content, &J::Application::sendersAttemptFinished, &app,
                         [&](const Attempt &result) { results.append(result); });
        const auto first     = content.requestSendersTracked(J::Origin::None);
        auto       oldUpdate = consume(content);
        const auto second    = content.requestSendersTracked(J::Origin::Responder);
        check(first && second > first, "request revisions are not monotonic");
        acknowledge(oldUpdate, &failure);
        check(results.size() == 1 && results[0].revision == first && results[0].target == J::Origin::None
                  && results[0].outcome == Attempt::Outcome::Rejected && results[0].error,
              "failure did not report the failed attempt's identity and error");
        auto newUpdate = consume(content);
        acknowledge(oldUpdate, &success);
        check(results.size() == 1 && content.senders() == J::Origin::Both, "stale ACK mutated newer attempt");
        acknowledge(newUpdate, &success);
        check(results.size() == 2 && results[1].id > results[0].id && results[1].revision == second
                  && results[1].outcome == Attempt::Outcome::Accepted && !results[1].error
                  && content.senders() == J::Origin::Responder,
              "accepted attempt lost its revision or negotiated target");
    }

    // A new, identical explicit request is a new scheduling revision too. An
    // error for its predecessor must not silently discard it.
    {
        TestApplication content(&session);
        content.activate();
        content.requestSendersTracked(J::Origin::None);
        auto       first  = consume(content);
        const auto latest = content.requestSendersTracked(J::Origin::None);
        acknowledge(first, &failure);
        auto    second    = consume(content);
        quint64 completed = 0;
        QObject::connect(&content, &J::Application::sendersAttemptFinished, &app,
                         [&](const Attempt &result) { completed = result.revision; });
        acknowledge(second, &success);
        check(completed == latest, "generic failure discarded a newer identical revision");
    }

    // Task timeouts have a local status code, not a parsed remote stanza error.
    {
        class TimedOut : public Task {
        public:
            explicit TimedOut(Task *parent) : Task(parent) { onTimeout(); }
        } timeout(client.rootTask());
        TestApplication content(&session);
        content.activate();
        content.requestSenders(J::Origin::None);
        const auto update   = consume(content);
        bool       notified = false;
        QObject::connect(&content, &J::Application::sendersAttemptFinished, &app, [&](const Attempt &result) {
            notified = result.outcome == Attempt::Outcome::TimedOut && result.error
                && result.error->condition == Stanza::Error::ErrorCond::RemoteServerTimeout;
        });
        acknowledge(update, &timeout);
        check(notified, "local IQ timeout was not classified explicitly");
    }

    // Completion is after cleanup, so even synchronous consumption of a new
    // request from a completion slot cannot be overwritten by the old callback.
    {
        TestApplication content(&session);
        content.activate();
        J::OutgoingUpdate next;
        int               completions = 0;
        QObject::connect(&content, &J::Application::sendersAttemptFinished, &app, [&](const Attempt &) {
            if (++completions == 1) {
                content.requestSenders(J::Origin::Responder);
                next = consume(content);
            }
        });
        content.requestSenders(J::Origin::None);
        const auto first = consume(content);
        acknowledge(first, &failure);
        acknowledge(next, &success);
        check(completions == 2 && content.senders() == J::Origin::Responder, "reentrant completion lost new attempt");
    }

    // Finishing cancels once. Deletion from the completion slot is safe, and a
    // saved callback cannot dereference a deleted application.
    {
        auto content = std::make_unique<TestApplication>(&session);
        content->activate();
        content->requestSenders(J::Origin::None);
        const auto update      = consume(*content);
        int        completions = 0;
        QObject::connect(content.get(), &J::Application::sendersAttemptFinished, &app, [&](const Attempt &result) {
            check(result.outcome == Attempt::Outcome::Cancelled, "finishing did not cancel attempt");
            ++completions;
        });
        content->setState(J::State::Finishing);
        acknowledge(update, &success);
        content.reset();
        acknowledge(update, &success);
        check(completions == 1, "cancelled attempt completed twice");
    }
    {
        auto content = std::make_unique<TestApplication>(&session);
        content->activate();
        content->requestSenders(J::Origin::None);
        const auto update = consume(*content);
        QObject::connect(content.get(), &J::Application::sendersAttemptFinished, &app,
                         [&](const Attempt &) { content.reset(); });
        acknowledge(update, &success);
        check(!content, "completion deletion fixture did not run");
        acknowledge(update, &failure);
    }
    return 0;
}
