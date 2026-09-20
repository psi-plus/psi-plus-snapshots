#include "../../src/xmpp/xmpp-im/jingle-ice-group_p.h"

#include <QCoreApplication>
#include <QPointer>

using namespace XMPP::Jingle;
using namespace XMPP::Jingle::ICE;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static GroupNegotiation::TransportParameters transportParameters()
{
    return { QStringLiteral("ufrag"), QStringLiteral("password"), QStringLiteral("sha-256"),
             QByteArray::fromHex("01020304"), QStringLiteral("actpass") };
}

static GroupNegotiation::Member member(const char *name)
{
    return { ContentKey { QString::fromLatin1(name), Origin::Initiator },
             QStringLiteral("urn:xmpp:jingle:transports:ice:0"), true, transportParameters() };
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QList<GroupNegotiation::Member> members { member("audio"), member("video"), member("screen") };
    const QList<ContentGroup>             offer { { QStringLiteral("BUNDLE"),
                                                    { QStringLiteral("audio"), QStringLiteral("video") } } };
    const QList<ContentGroup>             answer { { QStringLiteral("BUNDLE"),
                                                     { QStringLiteral("video"), QStringLiteral("audio") } } };

    auto plan = GroupNegotiation::initialPlan(members, offer, answer);
    check(plan && plan->readyToCommit(), "valid group plan was not committable");

    ConnectionRegistry registry;
    auto               transaction = ConnectionGroupTransaction::commit(*plan, registry);
    check(transaction && transaction->size() == 3, "valid group plan did not commit all memberships");

    auto shared = transaction->connectionFor(members.at(0).content);
    auto video  = transaction->connectionFor(members.at(1).content);
    auto screen = transaction->connectionFor(members.at(2).content);
    check(shared && shared == video && shared != screen, "committed membership topology did not match the plan");
    check(transaction->associationIdFor(members.at(0).content) == transaction->associationIdFor(members.at(1).content)
              && transaction->associationIdFor(members.at(0).content)
                  != transaction->associationIdFor(members.at(2).content),
          "live association ids did not match planned grouping");

    QPointer<IceConnection> sharedGuard(shared);
    QPointer<IceConnection> screenGuard(screen);
    check(transaction->release(members.at(1).content), "owner membership could not be released");
    check(sharedGuard && transaction->connectionFor(members.at(0).content) == sharedGuard,
          "removing the negotiated owner destroyed a surviving member");
    check(transaction->release(members.at(0).content), "surviving shared membership could not be released");
    check(!sharedGuard && screenGuard, "last shared release affected the independent association");
    check(transaction->release(members.at(2).content), "independent membership could not be released");
    check(!screenGuard && registry.liveAssociationCount() == 0, "final release retained a network association");

    // A BUNDLE restart/transport-replace must be make-before-break. The
    // replacement association is fully allocated while the registry still
    // exposes the old generation, then the weak index flips atomically. Old
    // memberships continue to own the previous connection until explicitly
    // retired by the higher-level signaling/runtime coordinator.
    {
        ConnectionRegistry replacementRegistry;
        auto current = ConnectionGroupTransaction::stageBundled(
            *plan, replacementRegistry, QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
        check(current && current->size() == 2 && replacementRegistry.liveAssociationCount() == 1,
              "failed to seed active BUNDLE replacement generation");

        const auto oldId = current->associationIdFor(members.at(0).content);
        auto *oldConnection = current->connectionFor(members.at(0).content);
        QPointer<IceConnection> oldGuard(oldConnection);

        auto replacement = ConnectionGroupTransaction::stageBundledReplacement(
            *plan, replacementRegistry, *current,
            QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
        check(replacement && replacement->isReplacement() && !replacement->replacementActive(),
              "failed to stage BUNDLE replacement generation");
        const auto newId = replacement->associationIdFor(members.at(0).content);
        auto *newConnection = replacement->connectionFor(members.at(0).content);
        QPointer<IceConnection> newGuard(newConnection);
        check(newId && newId != oldId && newConnection && newConnection != oldConnection,
              "replacement reused the active BUNDLE association");
        check(replacementRegistry.contains(oldId) && !replacementRegistry.contains(newId)
                  && replacementRegistry.liveAssociationCount() == 1,
              "staging replacement changed the registry-visible generation");

        check(replacement->activateReplacement(replacementRegistry)
                  && replacement->replacementActive(),
              "BUNDLE replacement generation did not activate atomically");
        check(!replacementRegistry.contains(oldId) && replacementRegistry.contains(newId)
                  && replacementRegistry.liveAssociationCount() == 1,
              "replacement activation exposed mixed BUNDLE generations");
        check(oldGuard && newGuard,
              "replacement activation broke make-before-break connection lifetime");

        check(replacement->rollbackReplacement(replacementRegistry)
                  && !replacement->replacementActive(),
              "BUNDLE replacement rollback failed");
        check(replacementRegistry.contains(oldId) && !replacementRegistry.contains(newId)
                  && oldGuard && newGuard,
              "replacement rollback did not restore the old registry generation");

        check(replacement->activateReplacement(replacementRegistry),
              "BUNDLE replacement could not be reactivated after rollback");
        check(replacement->finalizeReplacement(replacementRegistry)
                  && replacement->replacementFinalized(),
              "BUNDLE replacement generation did not finalize");
        check(!replacement->rollbackReplacement(replacementRegistry),
              "finalized BUNDLE replacement was still rollback-capable");
        check(oldGuard && newGuard,
              "finalizing replacement destroyed a generation still owned by transports");
        current.reset();
        check(!oldGuard && newGuard && replacementRegistry.contains(newId),
              "retiring old BUNDLE memberships destroyed the new generation");
        replacement.reset();
        check(!newGuard && replacementRegistry.liveAssociationCount() == 0,
              "replacement generation leaked after final membership release");
        replacementRegistry.prune();
    }

    // Replacing one BUNDLE group must not retire memberships belonging to
    // another group in the same Pad/current snapshot.
    {
        const QList<GroupNegotiation::Member> fourMembers {
            member("a1"), member("a2"), member("b1"), member("b2")
        };
        const QList<ContentGroup> twoGroups {
            { QStringLiteral("BUNDLE"), { QStringLiteral("a1"), QStringLiteral("a2") } },
            { QStringLiteral("BUNDLE"), { QStringLiteral("b1"), QStringLiteral("b2") } }
        };
        auto fullPlan = GroupNegotiation::initialPlan(fourMembers, twoGroups, twoGroups);
        check(fullPlan && fullPlan->readyToCommit(), "two-group plan was not committable");

        ConnectionRegistry multiRegistry;
        auto current = ConnectionGroupTransaction::stageBundled(
            *fullPlan, multiRegistry, QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
        check(current && current->size() == 4 && multiRegistry.liveAssociationCount() == 2,
              "failed to seed two independent BUNDLE associations");

        const auto a1 = fourMembers.at(0).content;
        const auto a2 = fourMembers.at(1).content;
        const auto b1 = fourMembers.at(2).content;
        const auto b2 = fourMembers.at(3).content;
        QPointer<IceConnection> oldA(current->connectionFor(a1));
        QPointer<IceConnection> stableB(current->connectionFor(b1));
        check(oldA && stableB && oldA != stableB && current->connectionFor(b2) == stableB,
              "two-group seed topology was wrong");

        const QList<GroupNegotiation::Member> aMembers { fourMembers.at(0), fourMembers.at(1) };
        const QList<ContentGroup> aGroup {
            { QStringLiteral("BUNDLE"), { QStringLiteral("a1"), QStringLiteral("a2") } }
        };
        auto aPlan = GroupNegotiation::initialPlan(aMembers, aGroup, aGroup);
        check(aPlan && aPlan->readyToCommit(), "first replacement plan invalid");
        auto replaceA = ConnectionGroupTransaction::stageBundledReplacement(
            *aPlan, multiRegistry, *current, QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
        check(replaceA && replaceA->activateReplacement(multiRegistry),
              "first group replacement did not activate");
        check(stableB && current->connectionFor(b1) == stableB && multiRegistry.liveAssociationCount() == 2,
              "activating first group replacement disturbed second group");
        check(replaceA->rollbackReplacement(multiRegistry),
              "first group replacement rollback failed");
        check(stableB && current->connectionFor(b1) == stableB && multiRegistry.liveAssociationCount() == 2,
              "rolling back first group replacement disturbed second group");

        check(replaceA->activateReplacement(multiRegistry) && replaceA->finalizeReplacement(multiRegistry),
              "first group replacement did not finalize");
        auto newA = replaceA->connectionFor(a1);
        QPointer<IceConnection> newAGuard(newA);
        replaceA->retainUnreplacedFrom(std::move(*current), QSet<ContentKey> { a1, a2 });
        current = std::move(replaceA);
        check(!oldA && newAGuard && stableB && current->connectionFor(a1) == newAGuard
                  && current->connectionFor(b1) == stableB && current->connectionFor(b2) == stableB
                  && multiRegistry.liveAssociationCount() == 2,
              "committing first group replacement retired unrelated group");

        const QList<GroupNegotiation::Member> bMembers { fourMembers.at(2), fourMembers.at(3) };
        const QList<ContentGroup> bGroup {
            { QStringLiteral("BUNDLE"), { QStringLiteral("b1"), QStringLiteral("b2") } }
        };
        auto bPlan = GroupNegotiation::initialPlan(bMembers, bGroup, bGroup);
        check(bPlan && bPlan->readyToCommit(), "second replacement plan invalid");
        auto replaceB = ConnectionGroupTransaction::stageBundledReplacement(
            *bPlan, multiRegistry, *current, QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
        check(replaceB && replaceB->activateReplacement(multiRegistry)
                  && replaceB->finalizeReplacement(multiRegistry),
              "second group replacement did not finalize");
        QPointer<IceConnection> newB(replaceB->connectionFor(b1));
        replaceB->retainUnreplacedFrom(std::move(*current), QSet<ContentKey> { b1, b2 });
        current = std::move(replaceB);
        check(newAGuard && newB && current->connectionFor(a1) == newAGuard
                  && current->connectionFor(b1) == newB && multiRegistry.liveAssociationCount() == 2,
              "committing second group replacement retired first group");

        current.reset();
        multiRegistry.prune();
        check(!newAGuard && !newB && multiRegistry.liveAssociationCount() == 0,
              "two-group replacement regression leaked associations");
    }

    auto refusalPlan = GroupNegotiation::initialPlan(members, offer, {});
    check(refusalPlan && refusalPlan->readyToCommit(), "BUNDLE refusal did not produce a committable fallback plan");
    auto refusal = ConnectionGroupTransaction::commit(*refusalPlan, registry);
    check(refusal && refusal->connectionFor(members.at(0).content) != refusal->connectionFor(members.at(1).content)
              && refusal->connectionFor(members.at(0).content) != refusal->connectionFor(members.at(2).content)
              && refusal->connectionFor(members.at(1).content) != refusal->connectionFor(members.at(2).content),
          "BUNDLE refusal reused an unnegotiated shared association");
    refusal.reset();
    check(registry.liveAssociationCount() == 0, "fallback transaction leaked associations");

    auto preflightMembers = members;
    preflightMembers[0].transportParameters.reset();
    preflightMembers[1].transportParameters.reset();
    auto preflightPlan = GroupNegotiation::initialPlan(preflightMembers, offer, answer);
    check(preflightPlan && !preflightPlan->readyToCommit(), "parameter-less plan unexpectedly became committable");
    check(!ConnectionGroupTransaction::commit(*preflightPlan, registry) && registry.liveAssociationCount() == 0,
          "preflight-only plan mutated the live registry");

    // Force failure on the last planned content. Audio/video associations are
    // created first, so a non-transactional implementation would leak them here.
    auto existingScreen = registry.create(members.at(2).content);
    check(existingScreen && registry.liveAssociationCount() == 1, "failed to seed rollback regression");
    check(!ConnectionGroupTransaction::commit(*plan, registry), "duplicate final member did not fail the commit");
    check(registry.liveAssociationCount() == 1 && existingScreen.membershipCount() == 1,
          "failed commit left a partially applied group");
    auto postRollbackAudio = registry.create(members.at(0).content);
    check(bool(postRollbackAudio), "rollback retained an earlier temporary audio membership");
    postRollbackAudio.reset();
    existingScreen.reset();
    check(registry.liveAssociationCount() == 0, "rollback regression leaked the seeded association");

    qInfo("Jingle ICE group commit regressions passed");
}
