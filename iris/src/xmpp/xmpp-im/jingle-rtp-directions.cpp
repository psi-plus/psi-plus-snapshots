// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-directions.h"
#include "jingle-rtp.h"
#include "jingle-session.h"
#include <QHash>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <utility>

namespace XMPP::Jingle::RTP {
struct DirectionController::Private {
    struct Entry {
        quint64                id = 0;
        QPointer<Application>  app;
        Policy                 policy;
        QMap<quint64, QString> constraints;
        quint64                queuedRevision   = 0;
        quint64                queuedPolicy     = 0;
        quint64                generation       = 0;
        quint64                queuedGeneration = 0;
        std::optional<Origin>  queuedTarget;
        int                    proposals = 0;
    };
    Pad                                               *pad;
    QHash<const Application *, std::shared_ptr<Entry>> entries;
    quint64                                            nextContent = 0, nextRevision = 0, nextToken = 0;
    bool                                               queued = false;
    QList<QPointer<DirectionOperation>>                operations;

    std::shared_ptr<Entry> adopt(DirectionController *q, Application *app)
    {
        if (!app || app->pad().data() != pad || !pad->session() || pad->session()->state() >= State::Finishing
            || app->state() >= State::Finishing)
            return {};
        if (entries.contains(app))
            return entries.value(app);
        auto entry = std::make_shared<Entry>();
        entry->id  = ++nextContent;
        entry->app = app;
        entries.insert(app, entry);
        QObject::connect(app, &QObject::destroyed, q, [this, q, app] {
            entries.remove(app); // tokens use incarnation IDs, not reused addresses
            q->schedule();
        });
        QObject::connect(app, &Application::sendersChanged, q, [q] { q->schedule(); });
        QObject::connect(app, &Application::stateChanged, q, [q] { q->schedule(); });
        QObject::connect(
            app, &Application::sendersAttemptFinished, q, [q, entry](const Application::SendersAttemptResult &result) {
                if (!entry->app || result.revision != entry->queuedRevision)
                    return;
                // Remove the disposable old target before TieBreaker invokes retry.
                // Recovery will derive a fresh local-bit proposal on the next turn.
                entry->app->cancelQueuedSenders(result.revision);
                entry->queuedRevision = 0;
                entry->queuedTarget.reset();
                using Outcome = Application::SendersAttemptResult::Outcome;
                if (result.outcome != Outcome::Accepted && entry->queuedGeneration == entry->generation) {
                    const bool tieBreak = result.error && result.error->condition == Stanza::Error::ErrorCond::Conflict
                        && result.error->appSpec.namespaceURI() == QLatin1String("urn:xmpp:jingle:errors:1")
                        && (result.error->appSpec.localName().isEmpty() ? result.error->appSpec.tagName()
                                                                        : result.error->appSpec.localName())
                            == QLatin1String("tie-break");
                    if (!tieBreak || entry->proposals >= 3) {
                        entry->policy.status  = Status::Failed;
                        entry->policy.failure = tieBreak           ? Failure::ReconciliationLimit
                            : result.outcome == Outcome::TimedOut  ? Failure::Timeout
                            : result.outcome == Outcome::Cancelled ? Failure::Cancelled
                                                                   : Failure::Signaling;
                        entry->policy.error   = result.error;
                    }
                }
                q->schedule();
            });
        return entry;
    }
};

DirectionController::DirectionController(Pad *pad) : QObject(pad), d(std::make_unique<Private>())
{
    d->pad = pad;
    // A caller may retain the Pad and Applications after the Session ends.
    // Do not rely solely on application state changes to retire their policies.
    if (auto session = pad->session()) {
        connect(session, &Session::terminated, this, &DirectionController::schedule);
        connect(session, &QObject::destroyed, this, &DirectionController::schedule);
    }
}
DirectionController::~DirectionController() = default;
DirectionController::Constraint::Constraint(DirectionController *controller, quint64 content, quint64 token) :
    controller_(controller), content_(content), token_(token)
{
}
DirectionController::Constraint::~Constraint()
{
    if (controller_)
        controller_->releaseConstraint(content_, token_);
}

quint64 DirectionController::setLocalSending(Application *app, bool enabled)
{
    auto entry = d->adopt(this, app);
    if (!entry)
        return 0;
    entry->policy.revision       = ++d->nextRevision;
    entry->policy.desiredSending = enabled;
    entry->policy.status         = Status::Pending;
    entry->policy.failure        = Failure::None;
    entry->policy.error.reset();
    ++entry->generation;
    entry->queuedTarget.reset();
    entry->proposals = 0;
    app->cancelQueuedSenders(entry->queuedRevision);
    schedule();
    return entry->policy.revision;
}

std::unique_ptr<DirectionController::Constraint> DirectionController::suspendLocalSending(Application   *app,
                                                                                          const QString &reason)
{
    auto entry = d->adopt(this, app);
    if (!entry)
        return {};
    const auto token = ++d->nextToken;
    entry->constraints.insert(token, reason);
    entry->policy.blockedReasons = entry->constraints.values();
    entry->policy.constrained    = true;
    entry->policy.status         = Status::Pending;
    entry->policy.failure        = Failure::None;
    entry->policy.error.reset();
    ++entry->generation;
    entry->queuedTarget.reset();
    entry->proposals = 0;
    app->cancelQueuedSenders(entry->queuedRevision);
    schedule();
    return std::unique_ptr<Constraint>(new Constraint(this, entry->id, token));
}

void DirectionController::releaseConstraint(quint64 content, quint64 token)
{
    for (const auto &entry : std::as_const(d->entries)) {
        if (entry->id != content || !entry->constraints.remove(token))
            continue;
        entry->policy.constrained    = !entry->constraints.isEmpty();
        entry->policy.blockedReasons = entry->constraints.values();
        entry->policy.status         = Status::Pending;
        entry->policy.failure        = Failure::None;
        entry->policy.error.reset();
        ++entry->generation;
        entry->queuedTarget.reset();
        entry->proposals = 0;
        if (entry->app)
            entry->app->cancelQueuedSenders(entry->queuedRevision);
        schedule();
        return;
    }
}

std::optional<DirectionController::Policy> DirectionController::policy(const Application *app) const
{
    const auto entry = d->entries.value(app);
    return entry ? std::optional<Policy>(entry->policy) : std::nullopt;
}

bool DirectionController::allowsLocalSending(const Application *app) const
{
    const auto entry = d->entries.value(app);
    return !entry
        || (entry->policy.desiredSending && !entry->policy.constrained && entry->policy.status != Status::Failed
            && entry->policy.status != Status::Finished);
}

void DirectionController::schedule()
{
    if (d->queued)
        return;
    d->queued = true;
    QTimer::singleShot(0, this, [this] {
        d->queued = false;
        reconcile();
    });
}

void DirectionController::reconcile()
{
    QPointer<DirectionController> guard(this);
    const auto                    entries = d->entries.values();
    for (const auto &entry : entries) {
        auto app = entry->app;
        if (!app)
            continue;
        if (app->state() >= State::Finishing || !d->pad->session() || d->pad->session()->state() >= State::Finishing) {
            app->cancelQueuedSenders(entry->queuedRevision);
            entry->policy.status = Status::Finished;
            continue;
        }
        if (entry->policy.status == Status::Failed)
            continue; // a new policy/constraint event, not an ACK loop, may retry
        const auto local       = d->pad->session()->role();
        const auto peer        = local == Origin::Initiator ? Origin::Responder : Origin::Initiator;
        const bool remoteSends = app->senders() == Origin::Both || app->senders() == peer;
        const bool localSends  = entry->policy.desiredSending && !entry->policy.constrained;
        const auto target = localSends ? (remoteSends ? Origin::Both : local) : (remoteSends ? peer : Origin::None);
        if (target == app->senders() && !app->sendersAttemptPending()) {
            app->cancelQueuedSenders(entry->queuedRevision);
            entry->queuedRevision = 0;
            entry->queuedTarget.reset();
            entry->policy.status = entry->policy.desiredSending && entry->policy.constrained
                ? Status::Blocked
                : (app->state() < State::Connecting ? Status::Pending : Status::Satisfied);
            continue;
        }
        if (entry->queuedTarget == target && entry->queuedPolicy == entry->policy.revision && entry->queuedRevision)
            continue;
        if (entry->proposals >= 3) {
            app->cancelQueuedSenders(entry->queuedRevision);
            entry->policy.status  = Status::Failed;
            entry->policy.failure = Failure::ReconciliationLimit;
            continue;
        }
        ++entry->proposals;
        entry->queuedTarget     = target;
        entry->queuedPolicy     = entry->policy.revision;
        entry->queuedGeneration = entry->generation;
        entry->policy.status
            = entry->policy.desiredSending && entry->policy.constrained ? Status::Blocked : Status::Pending;
        const auto generation = entry->generation;
        const auto revision   = app->requestSendersTracked(target);
        if (!guard)
            return;
        if (!app)
            continue;
        if (entry->generation != generation) {
            app->cancelQueuedSenders(revision);
            continue; // reentrant policy/constraint change already scheduled reconciliation
        }
        entry->queuedRevision = revision;
        if (!revision) {
            entry->policy.status  = Status::Failed;
            entry->policy.failure = Failure::RequestRejected;
        }
    }
    emit policyChanged();
}

std::unique_ptr<DirectionOperation> DirectionController::requestLocalSending(const QList<Request> &requests,
                                                                             int                   deadlineMs)
{
    using Error     = DirectionOperation::Error;
    Error rejection = Error::None;
    for (auto it = d->operations.begin(); it != d->operations.end();) {
        if (!*it || (*it)->state() != DirectionOperation::State::Pending)
            it = d->operations.erase(it);
        else
            ++it;
    }
    if (requests.isEmpty() || requests.size() > 64 || deadlineMs <= 0)
        rejection = Error::InvalidRequest;
    else if (d->operations.size() >= 32)
        rejection = Error::Capacity;

    QSet<Application *> seen;
    if (rejection == Error::None) {
        for (const auto &request : requests) {
            auto app = request.content;
            if (!app || app->pad().data() != d->pad || app->state() >= State::Finishing || !d->pad->session()
                || d->pad->session()->state() >= State::Finishing || seen.contains(app)) {
                rejection = Error::InvalidRequest;
                break;
            }
            seen.insert(app);
        }
    }
    QList<DirectionOperation::Item> items;
    if (requests.size() <= 64) {
        for (const auto &request : requests) {
            DirectionOperation::Item item;
            item.content = request.content;
            if (request.content)
                item.key = { request.content->contentName(), request.content->creator() };
            item.sending = request.sending;
            if (rejection == Error::None)
                item.revision = setLocalSending(request.content, request.sending);
            items.append(item);
        }
    }
    // setLocalSending only updates local state and queues work: no user callback
    // is invoked between batch validation and installation of all revisions.
    auto operation = std::unique_ptr<DirectionOperation>(new DirectionOperation(this, items, deadlineMs, rejection));
    if (rejection == Error::None)
        d->operations.append(operation.get());
    return operation;
}
}
