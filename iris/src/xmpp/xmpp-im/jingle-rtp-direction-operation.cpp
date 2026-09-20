// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-directions.h"
#include "jingle-rtp.h"
#include "jingle-session.h"
#include <QTimer>
#include <utility>

namespace XMPP::Jingle::RTP {
struct DirectionOperation::Private {
    QPointer<DirectionController> controller;
    QList<Item>                   items;
    State                         state     = State::Pending;
    Error                         error     = Error::None;
    Error                         rejection = Error::None;
    QTimer                        deadline;
    bool                          queued            = false;
    bool                          expired           = false;
    bool                          cancelled         = false;
    bool                          firstNotification = true;
};

DirectionOperation::DirectionOperation(DirectionController *controller, QList<Item> items, int deadlineMs,
                                       Error rejection) : d(std::make_unique<Private>())
{
    d->controller = controller;
    d->items      = std::move(items);
    d->rejection  = rejection;
    connect(controller, &DirectionController::policyChanged, this, &DirectionOperation::schedule);
    connect(controller, &QObject::destroyed, this, &DirectionOperation::schedule);
    d->deadline.setSingleShot(true);
    connect(&d->deadline, &QTimer::timeout, this, [this] {
        d->expired = true;
        schedule();
    });
    if (rejection == Error::None)
        d->deadline.start(deadlineMs);
    schedule();
}

DirectionOperation::~DirectionOperation() = default;
DirectionOperation::State       DirectionOperation::state() const { return d->state; }
DirectionOperation::Error       DirectionOperation::error() const { return d->error; }
QList<DirectionOperation::Item> DirectionOperation::items() const { return d->items; }

void DirectionOperation::cancel()
{
    if (d->state != State::Pending)
        return;
    d->cancelled = true;
    schedule();
}

void DirectionOperation::schedule()
{
    if (d->queued || d->state != State::Pending)
        return;
    d->queued = true;
    QTimer::singleShot(0, this, [this] {
        d->queued = false;
        evaluate();
    });
}

void DirectionOperation::evaluate()
{
    if (d->state != State::Pending)
        return;
    bool changed         = d->firstNotification;
    d->firstNotification = false;
    const auto previous  = d->items;
    if (d->rejection != Error::None) {
        d->state = State::Failed;
        d->error = d->rejection;
        for (auto &item : d->items) {
            item.state = ItemState::Failed;
            item.error = d->rejection;
        }
    } else if (d->cancelled || d->expired) {
        d->state = d->cancelled ? State::Cancelled : State::Failed;
        d->error = d->cancelled ? Error::ObservationEnded : Error::Timeout;
        for (auto &item : d->items) {
            if (item.state == ItemState::Satisfied)
                continue;
            item.state = d->cancelled ? ItemState::Cancelled : ItemState::Failed;
            item.error = d->error;
        }
    } else {
        bool failed = false, superseded = false, satisfied = true;
        for (auto &item : d->items) {
            auto       app    = item.content;
            const auto policy = d->controller && app ? d->controller->policy(app) : std::nullopt;
            item.error        = Error::None;
            item.failure      = DirectionController::Failure::None;
            item.signalingError.reset();
            item.blockedReasons.clear();
            if (!app || !policy || app->state() >= XMPP::Jingle::State::Finishing || !app->pad()->session()
                || app->pad()->session()->state() >= XMPP::Jingle::State::Finishing) {
                item.state = ItemState::Failed;
                item.error = Error::ContentGone;
                failed     = true;
            } else if (policy->revision != item.revision) {
                item.state = ItemState::Superseded;
                superseded = true;
            } else if (policy->status == DirectionController::Status::Failed) {
                item.state          = ItemState::Failed;
                item.error          = Error::PolicyFailure;
                item.failure        = policy->failure;
                item.signalingError = policy->error;
                failed              = true;
            } else if (policy->constrained && item.sending) {
                item.state          = ItemState::Blocked;
                item.blockedReasons = policy->blockedReasons;
            } else {
                const auto role       = app->pad()->session()->role();
                const bool localSends = app->senders() == Origin::Both || app->senders() == role;
                // Policy status is queued. Check the live predicate as well, so
                // an already changed peer bit/attempt cannot produce false success.
                item.state = policy->status == DirectionController::Status::Satisfied
                        && app->state() >= XMPP::Jingle::State::Connecting && !app->sendersAttemptPending()
                        && localSends == item.sending
                    ? ItemState::Satisfied
                    : ItemState::Pending;
            }
            satisfied &= item.state == ItemState::Satisfied;
        }
        if (failed) {
            d->state = State::Failed;
            for (const auto &item : std::as_const(d->items)) {
                if (item.state == ItemState::Failed) {
                    d->error = item.error;
                    break;
                }
            }
        } else if (superseded) {
            d->state = State::Superseded;
        } else if (satisfied) {
            d->state = State::Succeeded;
        }
        if (failed || superseded) {
            for (auto &item : d->items) {
                if (item.state == ItemState::Pending || item.state == ItemState::Blocked) {
                    item.state = ItemState::Cancelled;
                    item.error = Error::ObservationEnded;
                }
            }
        }
    }
    const bool terminal = d->state != State::Pending;
    if (terminal) {
        d->deadline.stop();
        changed = true;
    }
    for (int i = 0; i < d->items.size(); ++i) {
        const auto &old  = previous[i];
        const auto &item = d->items[i];
        changed |= old.state != item.state || old.error != item.error || old.failure != item.failure
            || old.blockedReasons != item.blockedReasons;
    }
    // All observable state is committed before any callback. A slot may cancel,
    // replace policy, or destroy this handle/the entire call synchronously.
    QPointer<DirectionOperation> guard(this);
    if (changed)
        emit progressChanged();
    if (guard && terminal)
        emit finished();
}
}
