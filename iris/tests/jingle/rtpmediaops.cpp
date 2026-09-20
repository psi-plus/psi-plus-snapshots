// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <iris/jingle-rtp.h>

namespace R = XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void pump()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

static R::Description description(const QString &media)
{
    R::Description result;
    result.media   = media;
    result.rtcpMux = true;
    R::PayloadType payload;
    payload.id        = media == QLatin1String("audio") ? 111 : 96;
    payload.name      = media == QLatin1String("audio") ? QStringLiteral("opus") : QStringLiteral("VP8");
    payload.clockrate = media == QLatin1String("audio") ? 48000 : 90000;
    payload.channels  = media == QLatin1String("audio") ? 2 : 1;
    result.payloads.append(payload);
    return result;
}

class Endpoint : public R::MediaEndpoint {
public:
    explicit Endpoint(QString media) : media(std::move(media)) { }
    R::Description                localOffer() const override { return description(media); }
    std::optional<R::Description> makeAnswer(const R::Description &offer) const override { return offer; }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &local, const R::Description &remote) override
    {
        ++configured;
        return local.media == media && remote.media == media;
    }
    void    stop() override { }
    QString media;
    int     configured = 0;
};

class AsyncSession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return std::make_unique<Endpoint>(media);
    }

    void expireDeadline(R::MediaOperation::Id id = 0) { mediaOperationDeadlineExpired(id ? id : deadlineId); }

    int                   localStarts = 0, answerStarts = 0, applyStarts = 0, cancels = 0, timeouts = 0;
    R::MediaOperation::Id lastId     = 0;
    R::MediaOperation::Id deadlineId = 0;
    int                   deadlineMs = 0;
    PrepareCompletion     prepareCompletion;
    ApplyCompletion       applyCompletion;

protected:
    void beginPrepareLocalOffer(R::MediaOperation::Id id, R::MediaEndpoint *, PrepareCompletion completion) override
    {
        check(!prepareCompletion && !applyCompletion, "media operations overlapped");
        ++localStarts;
        lastId            = id;
        prepareCompletion = std::move(completion);
    }
    void beginPrepareAnswer(R::MediaOperation::Id id, R::MediaEndpoint *, const R::Description &remote,
                            PrepareCompletion completion) override
    {
        check(remote.media == QLatin1String("audio"), "remote answer snapshot changed");
        check(!prepareCompletion && !applyCompletion, "media operations overlapped");
        ++answerStarts;
        lastId            = id;
        prepareCompletion = std::move(completion);
    }
    void beginApplyNegotiation(R::MediaOperation::Id id, R::MediaEndpoint *, const R::Description &local,
                               const R::Description &remote, ApplyCompletion completion) override
    {
        check(local.media == QLatin1String("audio") && remote.media == QLatin1String("audio"),
              "apply snapshots changed");
        check(!prepareCompletion && !applyCompletion, "media operations overlapped");
        ++applyStarts;
        lastId          = id;
        applyCompletion = std::move(completion);
    }
    void cancelMediaOperation(R::MediaOperation::Id id) override
    {
        check(id == lastId, "wrong media operation cancelled");
        ++cancels;
        prepareCompletion = {};
        applyCompletion   = {};
    }
    void timeoutMediaOperation(R::MediaOperation::Id id) override
    {
        check(id == lastId, "wrong media operation timed out");
        ++timeouts;
        prepareCompletion = {};
        applyCompletion   = {};
    }
    void armMediaOperationDeadline(R::MediaOperation::Id id, int timeoutMs) override
    {
        check(id != 0 && timeoutMs > 0, "invalid media operation deadline");
        deadlineId = id;
        deadlineMs = timeoutMs;
    }
    void disarmMediaOperationDeadline(R::MediaOperation::Id id) override
    {
        if (deadlineId != id)
            return;
        deadlineId = 0;
        deadlineMs = 0;
    }
};

class LegacySession : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return std::make_unique<Endpoint>(media);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication eventLoop(argc, argv);
    Endpoint         audio(QStringLiteral("audio"));
    AsyncSession     session;

    int  localCallbacks = 0, answerCallbacks = 0, applyCallbacks = 0;
    auto local = session.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id id, std::optional<R::Description> result, R::MediaError error) {
            check(id != 0 && result && !error && result->media == QLatin1String("audio"),
                  "local offer completion corrupted");
            ++localCallbacks;
        });
    auto answer = session.prepareAnswer(
        &audio, description(QStringLiteral("audio")),
        [&](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { ++answerCallbacks; });
    auto pendingApply
        = session.applyNegotiation(&audio, description(QStringLiteral("audio")), description(QStringLiteral("audio")),
                                   [&](R::MediaOperation::Id, R::MediaError) { ++applyCallbacks; });
    check(local && answer && pendingApply && local->id() && answer->id() && pendingApply->id()
              && local->id() != answer->id() && answer->id() != pendingApply->id(),
          "media operation ids are not unique and non-zero");
    check(session.localStarts == 0 && localCallbacks == 0, "media operation started or completed inline");

    pump();
    check(session.localStarts == 1 && session.answerStarts == 0 && session.applyStarts == 0,
          "media session did not serialize initial operations");
    auto firstCompletion      = session.prepareCompletion;
    session.prepareCompletion = {};
    firstCompletion(description(QStringLiteral("audio")), {});
    firstCompletion(description(QStringLiteral("audio")), {}); // duplicate backend completion must be ignored
    check(localCallbacks == 0, "media completion callback ran inline");
    pump();
    check(localCallbacks == 1 && session.answerStarts == 1 && session.applyStarts == 0,
          "one-shot completion or serialized start failed");

    // Cancelling queued work never reaches the backend.
    const int cancelsBeforeQueued = session.cancels;
    pendingApply->cancel();
    check(session.cancels == cancelsBeforeQueued, "queued media operation invoked backend cancellation");

    // Cancelling active work revokes completion delivery even if the backend races
    // a stale result after cancellation.
    auto staleAnswer = session.prepareCompletion;
    check(bool(staleAnswer), "active answer completion missing");
    answer->cancel();
    check(session.cancels == cancelsBeforeQueued + 1, "active media operation was not cancelled");
    staleAnswer(description(QStringLiteral("audio")), {});
    pump();
    check(answerCallbacks == 0 && session.applyStarts == 0, "cancelled/stale media callback was delivered");

    R::MediaOperation::Id applyId = 0;
    auto                  apply
        = session.applyNegotiation(&audio, description(QStringLiteral("audio")), description(QStringLiteral("audio")),
                                   [&](R::MediaOperation::Id id, R::MediaError error) {
                                       check(applyId && id == applyId && !error, "apply completion corrupted");
                                       ++applyCallbacks;
                                   });
    check(bool(apply), "apply operation handle missing");
    applyId = apply->id();
    pump();
    check(session.applyStarts == 1 && bool(session.applyCompletion), "apply operation did not start");
    auto applyDone          = session.applyCompletion;
    session.applyCompletion = {};
    applyDone({});
    applyDone({});
    check(applyCallbacks == 0, "apply callback ran inline");
    pump();
    check(applyCallbacks == 1, "duplicate apply completion was delivered");

    // A malformed backend completion is an explicit failure, never an empty
    // successful answer.
    int  invalidCallbacks = 0;
    auto invalid          = session.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(!result && error.code == R::MediaError::Code::Backend, "empty successful preparation was accepted");
            ++invalidCallbacks;
        });
    pump();
    auto invalidDone          = session.prepareCompletion;
    session.prepareCompletion = {};
    invalidDone({}, {});
    pump();
    check(invalidCallbacks == 1, "invalid preparation completion was lost");

    // Destroying the handle cancels active work. cancelAll() uses the same backend
    // hook while the derived MediaSession is alive.
    auto disposable = session.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    pump();
    const int beforeHandleDrop = session.cancels;
    disposable.reset();
    check(session.cancels == beforeHandleDrop + 1, "operation handle destruction did not cancel backend work");

    auto active = session.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    pump();
    const int beforeCancelAll = session.cancels;
    session.cancelAll();
    check(session.cancels == beforeCancelAll + 1, "cancelAll did not cancel active backend work");
    active.reset(); // already cancelled by the session: must not hit backend again
    check(session.cancels == beforeCancelAll + 1, "cancelled session operation was cancelled twice");

    // Deadlines use a controlled clock seam. A silent backend receives a distinct
    // timeout hook, while the live caller receives exactly one queued Timeout.
    AsyncSession            timed;
    R::MediaOperationPolicy timedPolicy;
    timedPolicy.prepareDeadlineMs    = 41;
    timedPolicy.applyDeadlineMs      = 23;
    timedPolicy.maxPendingOperations = 2;
    check(timed.setOperationPolicy(timedPolicy), "idle media operation policy was rejected");
    int           timedCallbacks = 0;
    R::MediaError timedError;
    auto          timedOffer = timed.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(!result, "timed-out preparation returned a result");
            timedError = std::move(error);
            ++timedCallbacks;
        });
    pump();
    check(timedOffer && timed.deadlineId == timedOffer->id() && timed.deadlineMs == 41,
          "prepare deadline was not armed from policy");
    auto lateAfterTimeout = timed.prepareCompletion;
    timed.expireDeadline();
    check(timed.timeouts == 1 && timed.cancels == 0 && timedCallbacks == 0,
          "timeout was not distinct from cancellation or callback ran inline");
    pump();
    check(timedCallbacks == 1 && timedError.code == R::MediaError::Code::Timeout,
          "silent backend did not produce one explicit timeout");
    lateAfterTimeout(description(QStringLiteral("audio")), {});
    pump();
    check(timedCallbacks == 1, "late completion after timeout was delivered");

    // Caller cancellation wins even if a stale deadline event arrives afterwards.
    AsyncSession cancelBeforeTimeout;
    check(cancelBeforeTimeout.setOperationPolicy(timedPolicy), "cancel timeout policy rejected");
    int  cancelledTimeoutCallbacks = 0;
    auto cancelledTimeout          = cancelBeforeTimeout.prepareLocalOffer(
        &audio,
        [&](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { ++cancelledTimeoutCallbacks; });
    pump();
    const auto cancelledId = cancelledTimeout->id();
    cancelledTimeout->cancel();
    cancelBeforeTimeout.expireDeadline(cancelledId);
    pump();
    check(cancelledTimeoutCallbacks == 0 && cancelBeforeTimeout.cancels == 1 && cancelBeforeTimeout.timeouts == 0,
          "deadline revived a caller-cancelled operation");

    // Completion arrival claims the operation and disarms its deadline before the
    // public callback is queued. At the opposite ordering, timeout wins and the
    // later backend completion is stale. Both cases deliver exactly one result.
    AsyncSession completionWins;
    check(completionWins.setOperationPolicy(timedPolicy), "completion boundary policy rejected");
    int  completionWinsCallbacks = 0;
    auto completionWinsOffer     = completionWins.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(result && !error, "completion did not win deadline boundary");
            ++completionWinsCallbacks;
        });
    pump();
    auto completionAtBoundary        = completionWins.prepareCompletion;
    completionWins.prepareCompletion = {};
    completionAtBoundary(description(QStringLiteral("audio")), {});
    completionWins.expireDeadline(completionWinsOffer->id());
    pump();
    check(completionWinsCallbacks == 1 && completionWins.timeouts == 0,
          "deadline overrode an already-arrived completion");

    AsyncSession timeoutWins;
    check(timeoutWins.setOperationPolicy(timedPolicy), "timeout boundary policy rejected");
    int           timeoutWinsCallbacks = 0;
    R::MediaError timeoutWinsError;
    auto          timeoutWinsOffer = timeoutWins.prepareLocalOffer(
        &audio, [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(!result, "timeout boundary returned a preparation result");
            timeoutWinsError = std::move(error);
            ++timeoutWinsCallbacks;
        });
    pump();
    auto completionAfterBoundary = timeoutWins.prepareCompletion;
    timeoutWins.expireDeadline(timeoutWinsOffer->id());
    completionAfterBoundary(description(QStringLiteral("audio")), {});
    pump();
    check(timeoutWinsCallbacks == 1 && timeoutWinsError.code == R::MediaError::Code::Timeout
              && timeoutWins.timeouts == 1,
          "completion revived an already-timed-out operation");

    // Apply has its own deadline and shares the same exact-once timeout contract.
    AsyncSession applyTimeout;
    check(applyTimeout.setOperationPolicy(timedPolicy), "apply timeout policy rejected");
    int           applyTimeoutCallbacks = 0;
    R::MediaError applyTimeoutError;
    auto          timedApply = applyTimeout.applyNegotiation(&audio, description(QStringLiteral("audio")),
                                                             description(QStringLiteral("audio")),
                                                             [&](R::MediaOperation::Id, R::MediaError error) {
                                                        applyTimeoutError = std::move(error);
                                                        ++applyTimeoutCallbacks;
                                                    });
    pump();
    check(timedApply && applyTimeout.deadlineId == timedApply->id() && applyTimeout.deadlineMs == 23,
          "apply deadline was not armed from policy");
    applyTimeout.expireDeadline();
    pump();
    check(applyTimeoutCallbacks == 1 && applyTimeoutError.code == R::MediaError::Code::Timeout,
          "apply timeout was not delivered exactly once");

    // Pending work is explicitly bounded. Rejection returns no operation handle,
    // and policy cannot be mutated while any operation is queued or active.
    AsyncSession            bounded;
    R::MediaOperationPolicy boundedPolicy = timedPolicy;
    boundedPolicy.maxPendingOperations    = 1;
    check(bounded.setOperationPolicy(boundedPolicy), "bounded queue policy rejected");
    auto boundedFirst = bounded.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    check(bool(boundedFirst), "first bounded operation was rejected");
    auto rejectedBeforeStart = bounded.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    check(!rejectedBeforeStart, "pending queue exceeded configured bound before start");
    check(!bounded.setOperationPolicy(timedPolicy), "policy changed while work was queued");
    pump();
    auto boundedSecond = bounded.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    check(bool(boundedSecond), "one queued operation behind active work was rejected");
    auto boundedThird = bounded.prepareLocalOffer(
        &audio, [](R::MediaOperation::Id, std::optional<R::Description>, R::MediaError) { });
    check(!boundedThird, "active operation accumulated an unbounded pending queue");
    boundedFirst->cancel();
    pump();
    check(bounded.localStarts == 2, "bounded queued operation did not start after cancellation");
    boundedSecond->cancel();
    pump();

    // Compatibility fallback remains asynchronous from the caller's point of view.
    LegacySession legacy;
    Endpoint      legacyAudio(QStringLiteral("audio"));
    int           fallbackPrepared = 0;
    auto          fallbackOffer    = legacy.prepareLocalOffer(
        &legacyAudio, [&](R::MediaOperation::Id, std::optional<R::Description> result, R::MediaError error) {
            check(result && !error, "legacy localOffer fallback failed");
            ++fallbackPrepared;
        });
    check(fallbackPrepared == 0, "legacy preparation completed inline");
    pump();
    check(fallbackPrepared == 1, "legacy preparation fallback did not complete");

    int  fallbackApplied = 0;
    auto fallbackApply   = legacy.applyNegotiation(&legacyAudio, description(QStringLiteral("audio")),
                                                   description(QStringLiteral("audio")),
                                                   [&](R::MediaOperation::Id, R::MediaError error) {
                                                     check(!error, "legacy configure fallback failed");
                                                     ++fallbackApplied;
                                                 });
    check(fallbackApplied == 0 && legacyAudio.configured == 0, "legacy apply ran inline");
    pump();
    check(fallbackApplied == 1 && legacyAudio.configured == 1, "legacy configure fallback did not complete");

    qInfo("RTP media operation regressions passed");
}
