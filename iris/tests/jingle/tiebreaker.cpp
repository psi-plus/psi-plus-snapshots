// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDomDocument>
#include <functional>
#include <iris/jingle-tiebreaker.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static QDomElement jingle(QDomDocument &doc, const QString &name)
{
    auto el = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    el.setAttribute(QStringLiteral("name"), name);
    doc.appendChild(el);
    return el;
}

static void churnDom()
{
    for (int i = 0; i < 128; ++i) {
        QDomDocument doc;
        auto root = doc.createElement(QStringLiteral("churn"));
        root.setAttribute(QStringLiteral("n"), i);
        doc.appendChild(root);
    }
}

class Resolver : public J::TieBreaker::Resolver {
public:
    J::TieBreaker::Solution     solution     = J::TieBreaker::Solution::Continue;
    int                         resolveCalls = 0;
    int                         retryCalls   = 0;
    QString                     localName;
    QString                     remoteName;
    QString                     retryRemoteName;
    std::function<void()>       onResolve;
    std::function<void()>       onRetry;
    J::TieBreaker::RemoteResult remoteResult = J::TieBreaker::RemoteResult::Rejected;

    J::TieBreaker::Solution resolve(const QDomElement &local, const QDomElement &remote) override
    {
        ++resolveCalls;
        localName  = local.attribute(QStringLiteral("name"));
        remoteName = remote.attribute(QStringLiteral("name"));
        if (onResolve)
            onResolve();
        return solution;
    }

    void retry(const J::TieBreaker::RetryContext &context) override
    {
        ++retryCalls;
        localName       = context.localData.attribute(QStringLiteral("name"));
        retryRemoteName = context.remoteData.attribute(QStringLiteral("name"));
        remoteResult    = context.remoteResult;
        if (onRetry)
            onRetry();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication    app(argc, argv);
    const Stanza::Error error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::Conflict);

    // Resolvers are keyed by Action and receive the exact local/remote XML.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution         = J::TieBreaker::Solution::Postpone;
        auto         registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto   local  = jingle(localDoc, QStringLiteral("local"));
        const auto   remote = jingle(remoteDoc, QStringLiteral("remote"));
        const auto   tx     = tieBreaker.outgoingStarted(J::Action::ContentModify, local);

        check(tieBreaker.resolveIncoming(J::Action::TransportInfo, remote).solution
                  == J::TieBreaker::Solution::Continue,
              "unrelated action reached a content-modify resolver");
        const auto resolution = tieBreaker.resolveIncoming(J::Action::ContentModify, remote);
        check(resolution.solution == J::TieBreaker::Solution::Postpone && resolution.id != 0,
              "postpone resolver was not armed");
        check(resolver.resolveCalls == 1 && resolver.localName == QLatin1String("local")
                  && resolver.remoteName == QLatin1String("remote"),
              "resolver did not receive local/remote Jingle data");
        check(registration.isPostponed(), "registration did not expose postponed state");

        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
        tieBreaker.outgoingFinished(tx, error);
        check(resolver.retryCalls == 0, "retry ran before owner IQ callback finished");
        // The transaction is no longer in flight before that callback.
        check(tieBreaker.resolveIncoming(J::Action::ContentModify, remote).solution
                  == J::TieBreaker::Solution::Continue,
              "completed IQ remained visible as a simultaneous action");
        tieBreaker.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 1 && resolver.remoteResult == J::TieBreaker::RemoteResult::Applied
                  && resolver.retryRemoteName == QLatin1String("remote"),
              "failed postponed IQ did not retry with full collision context after owner callback");
        check(!registration.isPostponed(), "retry left registration postponed");
    }

    // Success means the peer accepted our local proposal: no retry is necessary.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution         = J::TieBreaker::Solution::Postpone;
        auto         registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto   tx
            = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("accepted")));
        const auto resolution
            = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        tieBreaker.outgoingFinished(tx, std::nullopt);
        tieBreaker.outgoingCallbacksFinished(tx);
        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Rejected);
        check(resolver.retryCalls == 0 && !registration.isPostponed(),
              "accepted local proposal incorrectly retried postponed intent");
    }

    // Break dominates earlier Postpone decisions and must not arm retry state.
    {
        J::TieBreaker tieBreaker;
        Resolver      postpone, breaker;
        postpone.solution                 = J::TieBreaker::Solution::Postpone;
        breaker.solution                  = J::TieBreaker::Solution::Break;
        auto         postponeRegistration = tieBreaker.registerResolver(J::Action::ContentModify, &postpone);
        auto         breakRegistration    = tieBreaker.registerResolver(J::Action::ContentModify, &breaker);
        QDomDocument localDoc, remoteDoc;
        tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("local")));
        const auto result
            = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        check(result.solution == J::TieBreaker::Solution::Break && result.id == 0, "Break did not dominate Postpone");
        check(postpone.resolveCalls == 1 && breaker.resolveCalls == 1,
              "Break short-circuited another resolver instead of evaluating the full action");
        check(!postponeRegistration.isPostponed() && !breakRegistration.isPostponed(),
              "Break armed postponed retry state");
        auto snapshot = result.localData;
        check(snapshot.attribute("name") == "local", "Break did not expose the winning IQ snapshot");
        snapshot.setAttribute("name", "caller-mutation");
        const auto again = tieBreaker.resolveIncoming(J::Action::ContentModify, remoteDoc.documentElement());
        check(again.localData.attribute("name") == "local", "Break outcome aliased live transaction data");
    }

    // Tie-break snapshots must own their DOM independently of the caller's
    // short-lived parser documents.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution         = J::TieBreaker::Solution::Postpone;
        auto         registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        quint64      tx           = 0;
        {
            QDomDocument localDoc;
            tx = tieBreaker.outgoingStarted(J::Action::ContentModify,
                                            jingle(localDoc, QStringLiteral("owned-local")));
        }
        quint64 resolutionId = 0;
        {
            QDomDocument remoteDoc;
            const auto resolution
                = tieBreaker.resolveIncoming(J::Action::ContentModify,
                                             jingle(remoteDoc, QStringLiteral("owned-remote")));
            check(resolution.solution == J::TieBreaker::Solution::Postpone && resolution.id,
                  "owned snapshot fixture did not postpone");
            resolutionId = resolution.id;
        }
        churnDom();
        tieBreaker.outgoingFinished(tx, error);
        tieBreaker.outgoingCallbacksFinished(tx);
        tieBreaker.incomingFinished(resolutionId, J::TieBreaker::RemoteResult::Rejected);
        check(resolver.retryCalls == 1 && resolver.localName == QLatin1String("owned-local")
                  && resolver.retryRemoteName == QLatin1String("owned-remote"),
              "late retry depended on destroyed caller DOM documents");
        Q_UNUSED(registration);
    }

    {
        J::TieBreaker tieBreaker;
        Resolver      breaker;
        breaker.solution         = J::TieBreaker::Solution::Break;
        auto registration       = tieBreaker.registerResolver(J::Action::ContentModify, &breaker);
        J::TieBreaker::Resolution result;
        {
            QDomDocument localDoc, remoteDoc;
            tieBreaker.outgoingStarted(J::Action::ContentModify,
                                       jingle(localDoc, QStringLiteral("break-owned")));
            result = tieBreaker.resolveIncoming(J::Action::ContentModify,
                                                jingle(remoteDoc, QStringLiteral("remote")));
        }
        tieBreaker.clear();
        churnDom();
        check(result.solution == J::TieBreaker::Solution::Break
                  && result.localData.attribute(QStringLiteral("name")) == QLatin1String("break-owned"),
              "Break result depended on destroyed tie-break DOM documents");
        Q_UNUSED(registration);
    }

    // Remote processing may finish after our error. retry waits for both facts.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution         = J::TieBreaker::Solution::Postpone;
        auto         registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto tx = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("local")));
        const auto resolution
            = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        tieBreaker.outgoingFinished(tx, error);
        tieBreaker.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 0, "retry ran before remote processing completed");
        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Rejected);
        check(resolver.retryCalls == 1 && resolver.remoteResult == J::TieBreaker::RemoteResult::Rejected,
              "remote rejection was not delivered to retry");
    }

    // Destroying a registration or clearing the Session state cancels callbacks.
    {
        J::TieBreaker tieBreaker;
        Resolver      resolver;
        resolver.solution                        = J::TieBreaker::Solution::Postpone;
        J::TieBreaker::Registration registration = tieBreaker.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument                localDoc, remoteDoc;
        const auto tx = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc, QStringLiteral("local")));
        const auto resolution
            = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, QStringLiteral("remote")));
        check(registration.isPostponed(), "teardown test did not arm postpone");
        registration = {};
        tieBreaker.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
        tieBreaker.outgoingFinished(tx, error);
        tieBreaker.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 0, "destroyed resolver registration was called");

        Resolver second;
        second.solution                 = J::TieBreaker::Solution::Postpone;
        auto         secondRegistration = tieBreaker.registerResolver(J::Action::ContentModify, &second);
        QDomDocument localDoc2, remoteDoc2;
        const auto tx2 = tieBreaker.outgoingStarted(J::Action::ContentModify, jingle(localDoc2, QStringLiteral("two")));
        const auto resolution2
            = tieBreaker.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc2, QStringLiteral("remote-two")));
        check(secondRegistration.isPostponed(), "clear test did not arm postpone");
        tieBreaker.clear();
        tieBreaker.incomingFinished(resolution2.id, J::TieBreaker::RemoteResult::Applied);
        tieBreaker.outgoingFinished(tx2, error);
        tieBreaker.outgoingCallbacksFinished(tx2);
        check(second.retryCalls == 0 && !secondRegistration.isPostponed(), "clear left stale postponed callbacks");
    }

    // Cancellation inside dispatch must stop the snapshot, including when the
    // coordinator itself is destroyed. Registrations may outlive the Session.
    for (bool destroy : { false, true }) {
        for (bool duringRetry : { false, true }) {
            auto     coordinator = std::make_unique<J::TieBreaker>();
            Resolver first, second;
            first.solution = second.solution = J::TieBreaker::Solution::Postpone;
            auto a                           = coordinator->registerResolver(J::Action::ContentModify, &first);
            auto b                           = coordinator->registerResolver(J::Action::ContentModify, &second);
            auto cancel                      = [&] {
                if (destroy)
                    coordinator.reset();
                else
                    coordinator->clear();
            };
            if (duringRetry)
                first.onRetry = cancel;
            else
                first.onResolve = cancel;
            QDomDocument localDoc, remoteDoc;
            const auto   tx       = coordinator->outgoingStarted(J::Action::ContentModify, jingle(localDoc, "local"));
            const auto resolution = coordinator->resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, "remote"));
            if (duringRetry) {
                coordinator->incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
                coordinator->outgoingFinished(tx, error);
                coordinator->outgoingCallbacksFinished(tx);
                check(first.retryCalls == 1 && second.retryCalls == 0, "cancellation did not stop retry dispatch");
            } else {
                check(first.resolveCalls == 1 && second.resolveCalls == 0,
                      "cancellation did not stop resolver dispatch");
                check(resolution.id == 0, "cancelled dispatch armed a resolution");
            }
            check(!a.isPostponed() && !b.isPostponed(), "cancelled dispatch left postponed registrations");
        }
    }

    // Stored DOM must not alias the caller's mutable node handles. The first
    // terminal outcome wins, even if a duplicate notification says otherwise.
    {
        J::TieBreaker coordinator;
        Resolver      resolver;
        resolver.solution         = J::TieBreaker::Solution::Postpone;
        auto         registration = coordinator.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        auto         local  = jingle(localDoc, "local");
        auto         remote = jingle(remoteDoc, "remote");
        const auto   tx     = coordinator.outgoingStarted(J::Action::ContentModify, local);
        local.setAttribute("name", "mutated");
        const auto resolution = coordinator.resolveIncoming(J::Action::ContentModify, remote);
        remote.setAttribute("name", "mutated");
        coordinator.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
        coordinator.incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Rejected);
        coordinator.outgoingFinished(tx, error);
        coordinator.outgoingFinished(tx, std::nullopt);
        coordinator.outgoingCallbacksFinished(tx);
        coordinator.outgoingCallbacksFinished(tx);
        check(resolver.retryCalls == 1 && resolver.localName == "local" && resolver.retryRemoteName == "remote"
                  && resolver.remoteResult == J::TieBreaker::RemoteResult::Applied,
              "snapshots or first terminal outcome were overwritten");
    }

    // Finishing the outgoing IQ inside resolve must invalidate its collision,
    // without retaining an iterator into the now removed transaction.
    {
        J::TieBreaker coordinator;
        Resolver      first, second;
        first.solution = second.solution = J::TieBreaker::Solution::Postpone;
        auto         a                   = coordinator.registerResolver(J::Action::ContentModify, &first);
        auto         b                   = coordinator.registerResolver(J::Action::ContentModify, &second);
        QDomDocument localDoc, remoteDoc;
        const auto   tx = coordinator.outgoingStarted(J::Action::ContentModify, jingle(localDoc, "local"));
        first.onResolve = [&] {
            coordinator.outgoingFinished(tx, std::nullopt);
            coordinator.outgoingCallbacksFinished(tx);
        };
        const auto result = coordinator.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, "remote"));
        check(result.id == 0 && second.resolveCalls == 0 && !a.isPostponed(),
              "completed transaction was used after a resolver callback");
    }
    // An unregistered sibling is skipped, even though it was in the snapshot.
    {
        J::TieBreaker coordinator;
        Resolver      first, second;
        auto          a = coordinator.registerResolver(J::Action::ContentModify, &first);
        auto          b = coordinator.registerResolver(J::Action::ContentModify, &second);
        first.onResolve = [&] { b = {}; };
        QDomDocument localDoc, remoteDoc;
        coordinator.outgoingStarted(J::Action::ContentModify, jingle(localDoc, "local"));
        coordinator.resolveIncoming(J::Action::ContentModify, jingle(remoteDoc, "remote"));
        check(second.resolveCalls == 0, "unregistered sibling was called from a dispatch snapshot");
    }

    // A peer cannot retain an unlimited history while our outgoing IQ waits.
    // Reject overload without disguising it as a tie-break or arming recovery.
    {
        J::TieBreaker coordinator;
        Resolver      resolver;
        resolver.solution         = J::TieBreaker::Solution::Postpone;
        auto         registration = coordinator.registerResolver(J::Action::ContentModify, &resolver);
        QDomDocument localDoc, remoteDoc;
        const auto   tx     = coordinator.outgoingStarted(J::Action::ContentModify, jingle(localDoc, "local"));
        auto         remote = jingle(remoteDoc, "remote");
        for (int i = 0; i < 64; ++i) {
            const auto result = coordinator.resolveIncoming(J::Action::ContentModify, remote);
            check(result.id != 0 && !result.error, "bounded collision storage rejected an available slot");
            coordinator.incomingFinished(result.id, J::TieBreaker::RemoteResult::Applied);
        }
        const auto overload = coordinator.resolveIncoming(J::Action::ContentModify, remote);
        check(overload.error && overload.id == 0 && overload.solution != J::TieBreaker::Solution::Break,
              "collision overload was not explicitly rejected");
        coordinator.outgoingFinished(tx, std::nullopt);
        coordinator.outgoingCallbacksFinished(tx);
        check(!registration.isPostponed() && resolver.retryCalls == 0, "success failed to drain bounded storage");
        coordinator.outgoingStarted(J::Action::ContentModify, jingle(localDoc, "next"));
        remote.setAttribute("large", QString(256 * 1024, QLatin1Char('x')));
        check(coordinator.resolveIncoming(J::Action::ContentModify, remote).error.has_value(),
              "oversized arbitration snapshot was accepted");
        remote.removeAttribute("large");
        auto child = remote;
        for (int i = 0; i < 66; ++i) {
            auto next = remoteDoc.createElement("nested");
            child.appendChild(next);
            child = next;
        }
        check(coordinator.resolveIncoming(J::Action::ContentModify, remote).error.has_value(),
              "deeply nested arbitration snapshot was accepted");
    }
    return 0;
}
