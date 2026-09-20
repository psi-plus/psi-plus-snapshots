// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_DIRECTIONS_H
#define JINGLE_RTP_DIRECTIONS_H

#include <QObject>
#include <QPointer>
#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle.h>
#include <memory>
#include <optional>

namespace XMPP::Jingle::RTP {
class Application;
class Pad;
class DirectionOperation;

// Single-writer local-send policy for one RTP Pad. This is durable policy, not
// an operation handle. All calls and token destruction occur on the Jingle thread.
// Once adopted, use this controller (not requestSenders) for that content's policy.
class IRIS_EXPORT DirectionController : public QObject {
    Q_OBJECT
public:
    enum class Status { Pending, Satisfied, Blocked, Failed, Finished };
    enum class Failure { None, Signaling, Timeout, Cancelled, RequestRejected, ReconciliationLimit };
    struct Policy {
        quint64                      revision       = 0;
        bool                         desiredSending = false;
        bool                         constrained    = false;
        Status                       status         = Status::Pending;
        Failure                      failure        = Failure::None;
        std::optional<Stanza::Error> error;
        QStringList                  blockedReasons;
    };

    class IRIS_EXPORT Constraint {
    public:
        ~Constraint();
        Constraint(const Constraint &)            = delete;
        Constraint &operator=(const Constraint &) = delete;

    private:
        friend class DirectionController;
        Constraint(DirectionController *, quint64 content, quint64 token);
        QPointer<DirectionController> controller_;
        quint64                       content_;
        quint64                       token_;
    };

    ~DirectionController() override;
    // Explicit local preference/consent; preserves the live peer sending bit.
    // Returns a policy revision (zero for a foreign/terminal content).
    quint64 setLocalSending(Application *, bool enabled);
    struct Request {
        Application *content = nullptr;
        bool         sending = false;
    };
    // Validate the entire batch before changing policy. Always returns an
    // observation handle, including for invalid requests and no-op updates.
    // Completion is queued; cancellation/destruction never rolls policy back.
    // At most 64 items and 32 pending observations per Pad; deadline must be > 0.
    std::unique_ptr<DirectionOperation> requestLocalSending(const QList<Request> &, int deadlineMs = 15000);
    // Every live token forbids sending. Creating the first policy via a token
    // starts with desiredSending=false; negotiated senders never implies consent.
    std::unique_ptr<Constraint> suspendLocalSending(Application *, const QString &reason = {});
    std::optional<Policy>       policy(const Application *) const;
    // Packet gate closes synchronously on policy/constraint changes. It cannot
    // bypass negotiated senders, and does not itself stop device capture.
    bool allowsLocalSending(const Application *) const;

signals:
    // Coalesced queued notification; callers query live policies. Not an IQ ACK
    // and not a one-shot logical operation completion.
    void policyChanged();

private:
    friend class Pad;
    explicit DirectionController(Pad *);
    struct Private;
    std::unique_ptr<Private> d;
    void                     releaseConstraint(quint64 content, quint64 token);
    void                     schedule();
    void                     reconcile();
};

// A finite observation of a policy revision, possibly spanning multiple IQs.
// Ownership belongs to the caller. A batch is not a distributed transaction:
// failure/supersession stops observation, not the unaffected siblings' policies.
class IRIS_EXPORT DirectionOperation : public QObject {
    Q_OBJECT
public:
    enum class State { Pending, Succeeded, Failed, Cancelled, Superseded };
    enum class ItemState { Pending, Blocked, Satisfied, Failed, Cancelled, Superseded };
    enum class Error { None, InvalidRequest, Capacity, ContentGone, PolicyFailure, Timeout, ObservationEnded };
    struct Item {
        QPointer<Application>        content;
        ContentKey                   key;
        quint64                      revision = 0;
        bool                         sending  = false;
        ItemState                    state    = ItemState::Pending;
        Error                        error    = Error::None;
        DirectionController::Failure failure  = DirectionController::Failure::None;
        std::optional<Stanza::Error> signalingError;
        QStringList                  blockedReasons;
    };
    ~DirectionOperation() override;
    State       state() const;
    Error       error() const;
    QList<Item> items() const;
    void        cancel(); // stop observing; cannot unsend an IQ or withdraw consent

signals:
    void progressChanged(); // query per-item snapshots; delivery is coalesced
    void finished();        // once, queued, after final snapshots are installed

private:
    friend class DirectionController;
    DirectionOperation(DirectionController *, QList<Item>, int deadlineMs, Error rejection);
    struct Private;
    std::unique_ptr<Private> d;
    void                     schedule();
    void                     evaluate();
};
}
#endif
