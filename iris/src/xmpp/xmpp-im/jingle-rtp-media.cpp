// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp.h"

#include <QThread>
#include <QTimer>

namespace XMPP::Jingle::RTP {
namespace {
    MediaError backendError(const QString &text) { return { MediaError::Code::Backend, text }; }

    MediaError unsupportedError(const QString &text) { return { MediaError::Code::Unsupported, text }; }

    MediaError timeoutError()
    {
        return { MediaError::Code::Timeout, QStringLiteral("Media backend operation timed out") };
    }
}

class MediaOperation::Private {
public:
    Private(Id id, MediaSession *session) : id(id), session(session) { }
    Id                     id;
    QPointer<MediaSession> session;
    bool                   cancelled = false;
};

MediaOperation::MediaOperation(Id id, MediaSession *session) : d(std::make_unique<Private>(id, session)) { }
MediaOperation::~MediaOperation() { cancel(); }
MediaOperation::Id MediaOperation::id() const { return d ? d->id : 0; }
void               MediaOperation::cancel()
{
    if (!d || d->cancelled)
        return;
    d->cancelled = true;
    if (d->session)
        d->session->cancelOperation(d->id);
    d->session.clear();
}

class MediaSession::Private {
public:
    enum class Kind { PrepareLocalOffer, PrepareAnswer, ApplyNegotiation };
    struct State {
        MediaOperation::Id         id = 0;
        Kind                       kind;
        MediaEndpoint             *endpoint = nullptr;
        std::optional<Description> local;
        std::optional<Description> remote;
        PrepareCallback            prepareCallback;
        ApplyCallback              applyCallback;
        bool                       completionQueued = false;
        bool                       timedOut         = false;
    };

    MediaOperation::Id allocateId()
    {
        ++nextId;
        if (!nextId)
            ++nextId;
        return nextId;
    }

    QList<std::shared_ptr<State>> pending;
    std::shared_ptr<State>        active;
    MediaOperationPolicy          policy;
    QTimer                        deadlineTimer;
    MediaOperation::Id            deadlineId     = 0;
    MediaOperation::Id            nextId         = 0;
    bool                          startScheduled = false;
};

MediaSession::MediaSession(QObject *parent) : QObject(parent), d(std::make_unique<Private>())
{
    d->deadlineTimer.setSingleShot(true);
    connect(&d->deadlineTimer, &QTimer::timeout, this, [this] {
        const auto id = d->deadlineId;
        if (id)
            mediaOperationDeadlineExpired(id);
    });
}
MediaSession::~MediaSession()
{
    // Pad calls cancelAll() before destruction while virtual dispatch to the
    // adapter is still valid. This final cleanup only suppresses queued delivery.
    d->deadlineTimer.stop();
    d->pending.clear();
    d->active.reset();
}

MediaOperationPolicy MediaSession::operationPolicy() const { return d->policy; }

bool MediaSession::setOperationPolicy(const MediaOperationPolicy &policy)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!policy.isValid() || d->active || !d->pending.isEmpty() || d->startScheduled)
        return false;
    d->policy = policy;
    return true;
}

std::unique_ptr<MediaOperation> MediaSession::prepareLocalOffer(MediaEndpoint *endpoint, PrepareCallback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!endpoint || !callback || d->pending.size() >= d->policy.maxPendingOperations)
        return {};
    auto state             = std::make_shared<Private::State>();
    state->id              = d->allocateId();
    state->kind            = Private::Kind::PrepareLocalOffer;
    state->endpoint        = endpoint;
    state->prepareCallback = std::move(callback);
    d->pending.append(state);
    auto operation = std::unique_ptr<MediaOperation>(new MediaOperation(state->id, this));
    scheduleNext();
    return operation;
}

std::unique_ptr<MediaOperation> MediaSession::prepareAnswer(MediaEndpoint *endpoint, const Description &remoteSnapshot,
                                                            PrepareCallback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!endpoint || !callback || d->pending.size() >= d->policy.maxPendingOperations)
        return {};
    auto state             = std::make_shared<Private::State>();
    state->id              = d->allocateId();
    state->kind            = Private::Kind::PrepareAnswer;
    state->endpoint        = endpoint;
    state->remote          = remoteSnapshot;
    state->prepareCallback = std::move(callback);
    d->pending.append(state);
    auto operation = std::unique_ptr<MediaOperation>(new MediaOperation(state->id, this));
    scheduleNext();
    return operation;
}

std::unique_ptr<MediaOperation> MediaSession::applyNegotiation(MediaEndpoint *endpoint, const Description &local,
                                                               const Description &remote, ApplyCallback callback)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!endpoint || !callback || d->pending.size() >= d->policy.maxPendingOperations)
        return {};
    auto state           = std::make_shared<Private::State>();
    state->id            = d->allocateId();
    state->kind          = Private::Kind::ApplyNegotiation;
    state->endpoint      = endpoint;
    state->local         = local;
    state->remote        = remote;
    state->applyCallback = std::move(callback);
    d->pending.append(state);
    auto operation = std::unique_ptr<MediaOperation>(new MediaOperation(state->id, this));
    scheduleNext();
    return operation;
}

void MediaSession::cancelOperation(MediaOperation::Id id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active && d->active->id == id) {
        disarmMediaOperationDeadline(id);
        d->active.reset();
        QPointer<MediaSession> guard(this);
        cancelMediaOperation(id);
        if (guard)
            scheduleNext();
        return;
    }
    for (auto it = d->pending.begin(); it != d->pending.end(); ++it) {
        if ((*it)->id == id) {
            d->pending.erase(it);
            scheduleNext();
            return;
        }
    }
}

void MediaSession::cancelAll()
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->pending.clear();
    if (!d->active)
        return;
    const auto id = d->active->id;
    disarmMediaOperationDeadline(id);
    d->active.reset();
    cancelMediaOperation(id);
}

void MediaSession::scheduleNext()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->startScheduled || d->active || d->pending.isEmpty())
        return;
    d->startScheduled = true;
    QPointer<MediaSession> guard(this);
    QTimer::singleShot(0, this, [guard] {
        if (!guard)
            return;
        guard->d->startScheduled = false;
        guard->startNext();
    });
}

void MediaSession::startNext()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->active || d->pending.isEmpty())
        return;
    auto state = d->pending.takeFirst();
    d->active  = state;

    QPointer<MediaSession> guard(this);
    const auto             id = state->id;
    const int              deadline
        = state->kind == Private::Kind::ApplyNegotiation ? d->policy.applyDeadlineMs : d->policy.prepareDeadlineMs;
    armMediaOperationDeadline(id, deadline);
    if (!guard || !d->active || d->active->id != id)
        return;

    switch (state->kind) {
    case Private::Kind::PrepareLocalOffer:
        beginPrepareLocalOffer(
            id, state->endpoint, [guard, id](std::optional<Description> result, MediaError error) mutable {
                if (!guard)
                    return;
                Q_ASSERT(QThread::currentThread() == guard->thread());
                if (!guard->claimCompletion(id))
                    return;
                QTimer::singleShot(0, guard,
                                   [guard, id, result = std::move(result), error = std::move(error)]() mutable {
                                       if (guard)
                                           guard->finishPrepared(id, std::move(result), std::move(error));
                                   });
            });
        break;
    case Private::Kind::PrepareAnswer:
        beginPrepareAnswer(id, state->endpoint, *state->remote,
                           [guard, id](std::optional<Description> result, MediaError error) mutable {
                               if (!guard)
                                   return;
                               Q_ASSERT(QThread::currentThread() == guard->thread());
                               if (!guard->claimCompletion(id))
                                   return;
                               QTimer::singleShot(
                                   0, guard,
                                   [guard, id, result = std::move(result), error = std::move(error)]() mutable {
                                       if (guard)
                                           guard->finishPrepared(id, std::move(result), std::move(error));
                                   });
                           });
        break;
    case Private::Kind::ApplyNegotiation:
        beginApplyNegotiation(id, state->endpoint, *state->local, *state->remote,
                              [guard, id](MediaError error) mutable {
                                  if (!guard)
                                      return;
                                  Q_ASSERT(QThread::currentThread() == guard->thread());
                                  if (!guard->claimCompletion(id))
                                      return;
                                  QTimer::singleShot(0, guard, [guard, id, error = std::move(error)]() mutable {
                                      if (guard)
                                          guard->finishApplied(id, std::move(error));
                                  });
                              });
        break;
    }
}

bool MediaSession::claimCompletion(MediaOperation::Id id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id || d->active->timedOut || d->active->completionQueued)
        return false;
    d->active->completionQueued = true;
    disarmMediaOperationDeadline(id);
    return true;
}

void MediaSession::finishPrepared(MediaOperation::Id id, std::optional<Description> result, MediaError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id || !d->active->completionQueued || d->active->timedOut
        || (d->active->kind != Private::Kind::PrepareLocalOffer && d->active->kind != Private::Kind::PrepareAnswer))
        return; // cancelled, stale, duplicate, timed out, or wrong-kind completion

    const bool hasError = bool(error);
    if (result.has_value() == hasError) {
        result.reset();
        error = backendError(QStringLiteral("Media backend returned an invalid preparation result"));
    }

    auto callback = std::move(d->active->prepareCallback);
    d->active.reset();
    QPointer<MediaSession> guard(this);
    if (callback)
        callback(id, std::move(result), std::move(error));
    if (guard)
        scheduleNext();
}

void MediaSession::finishApplied(MediaOperation::Id id, MediaError error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id || !d->active->completionQueued || d->active->timedOut
        || d->active->kind != Private::Kind::ApplyNegotiation)
        return; // cancelled, stale, duplicate, timed out, or wrong-kind completion
    auto callback = std::move(d->active->applyCallback);
    d->active.reset();
    QPointer<MediaSession> guard(this);
    if (callback)
        callback(id, std::move(error));
    if (guard)
        scheduleNext();
}

void MediaSession::finishTimedOut(MediaOperation::Id id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id || !d->active->timedOut || d->active->completionQueued)
        return;

    const auto kind            = d->active->kind;
    auto       prepareCallback = std::move(d->active->prepareCallback);
    auto       applyCallback   = std::move(d->active->applyCallback);
    d->active.reset();

    QPointer<MediaSession> guard(this);
    if (kind == Private::Kind::ApplyNegotiation) {
        if (applyCallback)
            applyCallback(id, timeoutError());
    } else if (prepareCallback) {
        prepareCallback(id, {}, timeoutError());
    }
    if (guard)
        scheduleNext();
}

void MediaSession::armMediaOperationDeadline(MediaOperation::Id id, int timeoutMs)
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->deadlineTimer.stop();
    d->deadlineId = id;
    d->deadlineTimer.start(timeoutMs);
}

void MediaSession::disarmMediaOperationDeadline(MediaOperation::Id id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->deadlineId != id)
        return;
    d->deadlineTimer.stop();
    d->deadlineId = 0;
}

void MediaSession::mediaOperationDeadlineExpired(MediaOperation::Id id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->active || d->active->id != id || d->active->timedOut || d->active->completionQueued)
        return;

    d->active->timedOut = true;
    disarmMediaOperationDeadline(id);

    // Queue the explicit operation error before the provider-specific timeout
    // action. A fail-closed provider may queue runtimeError to tear down sibling
    // applications, but the operation which actually expired gets its own error.
    QPointer<MediaSession> guard(this);
    QTimer::singleShot(0, this, [guard, id] {
        if (guard)
            guard->finishTimedOut(id);
    });
    if (guard)
        timeoutMediaOperation(id);
}

void MediaSession::beginPrepareLocalOffer(MediaOperation::Id, MediaEndpoint *endpoint, PrepareCompletion completion)
{
    if (!endpoint) {
        completion({}, backendError(QStringLiteral("Media endpoint disappeared")));
        return;
    }
    completion(endpoint->localOffer(), {});
}

void MediaSession::beginPrepareAnswer(MediaOperation::Id, MediaEndpoint *endpoint, const Description &remoteSnapshot,
                                      PrepareCompletion completion)
{
    if (!endpoint) {
        completion({}, backendError(QStringLiteral("Media endpoint disappeared")));
        return;
    }
    auto answer = endpoint->makeAnswer(remoteSnapshot);
    if (!answer) {
        completion({}, unsupportedError(QStringLiteral("Media backend rejected the remote offer")));
        return;
    }
    completion(std::move(answer), {});
}

void MediaSession::beginApplyNegotiation(MediaOperation::Id, MediaEndpoint *endpoint, const Description &local,
                                         const Description &remote, ApplyCompletion completion)
{
    if (!endpoint) {
        completion(backendError(QStringLiteral("Media endpoint disappeared")));
        return;
    }
    completion(endpoint->configure(local, remote)
                   ? MediaError {}
                   : backendError(QStringLiteral("Media backend failed to apply negotiated parameters")));
}

}
