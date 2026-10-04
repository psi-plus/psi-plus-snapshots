// Internal bridge from a validated GroupPlan to session-local ICE memberships.
#ifndef JINGLE_ICE_GROUP_P_H
#define JINGLE_ICE_GROUP_P_H

#include "jingle-group-negotiation_p.h"
#include "jingle-ice-connection_p.h"

#include <QSet>

#include <optional>
#include <vector>

namespace XMPP { namespace Jingle { namespace ICE {

    class ConnectionGroupTransaction {
    public:
        struct Entry {
            int                  planAssociationId = -1;
            ContentKey           content;
            ConnectionMembership membership;
        };

        ConnectionGroupTransaction()                                              = default;
        ConnectionGroupTransaction(const ConnectionGroupTransaction &)            = delete;
        ConnectionGroupTransaction &operator=(const ConnectionGroupTransaction &) = delete;
        ConnectionGroupTransaction(ConnectionGroupTransaction &&)                 = default;
        ConnectionGroupTransaction &operator=(ConnectionGroupTransaction &&)      = default;

        static std::optional<ConnectionGroupTransaction>
        stageBundled(const GroupPlan &plan, ConnectionRegistry &registry, const QString &transportNamespace)
        {
            ConnectionGroupTransaction result;
            for (const auto &association : plan.associations()) {
                if (!association.bundled || association.members.isEmpty()
                    || association.transportNamespace != transportNamespace)
                    continue;
                if (association.owner != association.members.first())
                    return std::nullopt;

                auto ownerMembership = registry.create(association.owner);
                if (!ownerMembership)
                    return std::nullopt;
                const auto liveAssociationId = ownerMembership.associationId();
                result.entries_.push_back(Entry { association.id, association.owner, std::move(ownerMembership) });

                for (qsizetype index = 1; index < association.members.size(); ++index) {
                    const auto &content    = association.members.at(index);
                    auto        membership = registry.attach(liveAssociationId, content);
                    if (!membership)
                        return std::nullopt;
                    result.entries_.push_back(Entry { association.id, content, std::move(membership) });
                }
            }
            return result;
        }

        // Stage additional logical contents onto an already-live association
        // without publishing membership yet. The returned transaction pins the
        // existing association strongly so a new Transport can prepare against
        // the same IceConnection before content-add/content-accept completes.
        // activateExtension() is the signaling commit point; destruction before
        // activation is a no-op on live membership/generation.
        static std::optional<ConnectionGroupTransaction>
        stageMembershipExtension(const ConnectionGroupTransaction &current, ConnectionRegistry &registry,
                                 const ContentKey &existingMember, const QList<ContentKey> &additions)
        {
            const auto associationId = current.associationIdFor(existingMember);
            if (!associationId || additions.isEmpty())
                return std::nullopt;

            auto state = registry.associations_.value(associationId).toStrongRef();
            if (!state || !state->connection || !state->members.contains(existingMember))
                return std::nullopt;

            QSet<ContentKey> seen;
            for (const auto &content : additions) {
                if (content.first.isEmpty()
                    || (content.second != Origin::Initiator && content.second != Origin::Responder)
                    || seen.contains(content) || current.associationIdFor(content) || registry.containsContent(content))
                    return std::nullopt;
                seen.insert(content);
            }

            ConnectionGroupTransaction result;
            result.isExtension_            = true;
            result.extensionAssociationId_ = associationId;
            result.extensionAnchor_        = existingMember;
            result.extensionState_         = std::move(state);
            result.extensionContents_      = additions;
            return result;
        }

        static std::optional<ConnectionGroupTransaction> commit(const GroupPlan &plan, ConnectionRegistry &registry)
        {
            if (!plan.readyToCommit())
                return std::nullopt;

            ConnectionGroupTransaction result;
            for (const auto &association : plan.associations()) {
                if (association.members.isEmpty() || association.owner != association.members.first())
                    return std::nullopt;

                auto ownerMembership = registry.create(association.owner);
                if (!ownerMembership)
                    return std::nullopt;
                const auto liveAssociationId = ownerMembership.associationId();
                result.entries_.push_back(Entry { association.id, association.owner, std::move(ownerMembership) });

                for (qsizetype index = 1; index < association.members.size(); ++index) {
                    const auto &content    = association.members.at(index);
                    auto        membership = registry.attach(liveAssociationId, content);
                    if (!membership)
                        return std::nullopt;
                    result.entries_.push_back(Entry { association.id, content, std::move(membership) });
                }
            }
            return result;
        }

        // Stage a new physical association for an already-active BUNDLE group
        // without touching the registry-visible generation. This is the
        // make-before-break primitive used by transport-replace/ICE restart.
        static std::optional<ConnectionGroupTransaction>
        stageBundledReplacement(const GroupPlan &plan, ConnectionRegistry &registry,
                                const ConnectionGroupTransaction &current, const QString &transportNamespace)
        {
            ConnectionGroupTransaction result;
            result.isReplacement_ = true;

            for (const auto &association : plan.associations()) {
                if (!association.bundled || association.members.isEmpty()
                    || association.transportNamespace != transportNamespace)
                    continue;
                if (association.owner != association.members.first())
                    return std::nullopt;

                const auto oldAssociationId = current.associationIdFor(association.owner);
                if (!oldAssociationId)
                    return std::nullopt;
                for (const auto &content : association.members) {
                    if (current.associationIdFor(content) != oldAssociationId)
                        return std::nullopt;
                }

                auto oldState = registry.associations_.value(oldAssociationId).toStrongRef();
                if (!oldState || oldState->members.size() != association.members.size())
                    return std::nullopt;
                for (const auto &content : association.members) {
                    if (!oldState->members.contains(content))
                        return std::nullopt;
                }

                auto newState        = QSharedPointer<ConnectionAssociationState>::create();
                newState->id         = registry.nextAssociationId_++;
                newState->connection = QSharedPointer<IceConnection>::create();
                for (const auto &content : association.members)
                    newState->members.insert(content);
                newState->connection->generation.membershipRevision += quint64(association.members.size());

                result.replacements_.push_back(
                    ReplacementAssociation { oldAssociationId, std::move(oldState), newState });

                for (const auto &content : association.members) {
                    result.entries_.push_back(
                        Entry { association.id, content, ConnectionMembership(newState, content) });
                }
            }

            if (result.replacements_.empty())
                return std::nullopt;
            return result;
        }

        bool activateReplacement(ConnectionRegistry &registry)
        {
            if (!isReplacement_ || replacementActive_)
                return false;

            // Revalidate the complete old generation before mutating the weak
            // registry index. Membership owners keep the old connections alive.
            for (const auto &replacement : replacements_) {
                if (registry.associations_.value(replacement.oldAssociationId).toStrongRef() != replacement.oldState)
                    return false;
                if (registry.associations_.contains(replacement.newState->id))
                    return false;
            }

            for (const auto &replacement : replacements_)
                registry.associations_.remove(replacement.oldAssociationId);
            for (const auto &replacement : replacements_)
                registry.associations_.insert(replacement.newState->id, replacement.newState.toWeakRef());

            replacementActive_ = true;
            return true;
        }

        bool rollbackReplacement(ConnectionRegistry &registry)
        {
            if (!isReplacement_ || !replacementActive_ || replacementFinalized_)
                return false;

            for (const auto &replacement : replacements_) {
                if (registry.associations_.value(replacement.newState->id).toStrongRef() != replacement.newState)
                    return false;
            }
            for (const auto &replacement : replacements_)
                registry.associations_.remove(replacement.newState->id);
            for (const auto &replacement : replacements_)
                registry.associations_.insert(replacement.oldAssociationId, replacement.oldState.toWeakRef());

            replacementActive_ = false;
            return true;
        }

        bool finalizeReplacement(ConnectionRegistry &registry)
        {
            if (!isReplacement_ || !replacementActive_ || replacementFinalized_)
                return false;

            // Finalization is the point of no return: signaling/runtime has
            // committed the new association generation, so the transaction no
            // longer owns a rollback reference to the old generation. Existing
            // old Transport memberships may still keep it alive until retired.
            for (const auto &replacement : replacements_) {
                if (registry.associations_.value(replacement.newState->id).toStrongRef() != replacement.newState)
                    return false;
            }
            for (auto &replacement : replacements_)
                replacement.oldState.clear();

            replacementFinalized_ = true;
            return true;
        }

        bool isReplacement() const { return isReplacement_; }
        bool replacementActive() const { return replacementActive_; }
        bool replacementFinalized() const { return replacementFinalized_; }

        bool isExtension() const { return isExtension_; }
        bool extensionActive() const { return extensionActive_; }
        bool extensionFinalized() const { return extensionFinalized_; }

        bool activateExtension(ConnectionRegistry &registry)
        {
            if (!isExtension_ || extensionActive_ || extensionFinalized_ || !extensionState_
                || !extensionState_->members.contains(extensionAnchor_)
                || registry.associations_.value(extensionAssociationId_).toStrongRef() != extensionState_)
                return false;

            for (const auto &content : std::as_const(extensionContents_)) {
                if (registry.containsContent(content) || extensionState_->members.contains(content))
                    return false;
            }

            for (const auto &content : std::as_const(extensionContents_)) {
                extensionState_->members.insert(content);
                ++extensionState_->connection->generation.membershipRevision;
                entries_.push_back(Entry { -1, content, ConnectionMembership(extensionState_, content) });
            }
            extensionActive_ = true;
            return true;
        }

        bool rollbackExtension()
        {
            if (!isExtension_ || extensionFinalized_)
                return false;
            if (extensionActive_) {
                // These entries contain only the newly-added memberships.
                // Their destructors remove the contents and advance membership
                // revision; the established association and its old members stay.
                entries_.clear();
                extensionActive_ = false;
            }
            extensionState_.clear();
            extensionContents_.clear();
            return true;
        }

        bool finalizeExtension(ConnectionGroupTransaction &current, ConnectionRegistry &registry)
        {
            if (!isExtension_ || !extensionActive_ || extensionFinalized_ || !extensionState_
                || registry.associations_.value(extensionAssociationId_).toStrongRef() != extensionState_
                || current.associationIdFor(extensionAnchor_) != extensionAssociationId_)
                return false;
            for (const auto &entry : entries_) {
                if (current.associationIdFor(entry.content))
                    return false;
            }
            for (auto &entry : entries_)
                current.entries_.push_back(std::move(entry));
            entries_.clear();
            extensionContents_.clear();
            extensionState_.clear();
            extensionFinalized_ = true;
            return true;
        }

        qsizetype size() const { return qsizetype(entries_.size()); }

        // Runtime teardown may be reentrant from a child QObject's destroyed
        // signal. A temporary pin lets the pad retire membership immediately
        // while deferring parent destruction until that callback has returned.
        QSharedPointer<IceConnection> pinConnection(const ContentKey &content) const
        {
            for (const auto &entry : entries_) {
                if (entry.content == content && entry.membership.state_)
                    return entry.membership.state_->connection;
            }
            return {};
        }

        IceConnection *connectionFor(const ContentKey &content) const
        {
            for (const auto &entry : entries_) {
                if (entry.content == content)
                    return entry.membership.connection();
            }
            if (isExtension_ && !extensionFinalized_ && extensionState_ && extensionContents_.contains(content))
                return extensionState_->connection.data();
            return nullptr;
        }

        quint64 associationIdFor(const ContentKey &content) const
        {
            for (const auto &entry : entries_) {
                if (entry.content == content)
                    return entry.membership.associationId();
            }
            if (isExtension_ && !extensionFinalized_ && extensionState_ && extensionContents_.contains(content))
                return extensionAssociationId_;
            return 0;
        }

        bool release(const ContentKey &content)
        {
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->content == content) {
                    entries_.erase(it);
                    return true;
                }
            }
            return false;
        }

        // Initial refusal of a one-member group can keep the same network as
        // an independent transport. Transfer ownership without detaching and
        // reattaching the content or changing its membership generation.
        ConnectionMembership takeMembership(const ContentKey &content)
        {
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->content == content) {
                    auto membership = std::move(it->membership);
                    entries_.erase(it);
                    return membership;
                }
            }
            return {};
        }

        // After a replacement generation has been finalized, keep memberships
        // belonging to unrelated associations from the previous current
        // snapshot. Only contents in replacedContents are retired.
        void retainUnreplacedFrom(ConnectionGroupTransaction &&previous, const QSet<ContentKey> &replacedContents)
        {
            Q_ASSERT(isReplacement_ && replacementFinalized_);
            for (auto &entry : previous.entries_) {
                if (!replacedContents.contains(entry.content))
                    entries_.push_back(std::move(entry));
            }
            previous.entries_.clear();

            // The transaction is now the ordinary current membership snapshot.
            // Replacement bookkeeping must not survive into a later restart.
            replacements_.clear();
            isReplacement_        = false;
            replacementActive_    = false;
            replacementFinalized_ = false;
        }

    private:
        struct ReplacementAssociation {
            quint64                                    oldAssociationId = 0;
            QSharedPointer<ConnectionAssociationState> oldState;
            QSharedPointer<ConnectionAssociationState> newState;
        };

        std::vector<Entry>                  entries_;
        std::vector<ReplacementAssociation> replacements_;
        bool                                isReplacement_        = false;
        bool                                replacementActive_    = false;
        bool                                replacementFinalized_ = false;

        bool                                       isExtension_            = false;
        bool                                       extensionActive_        = false;
        bool                                       extensionFinalized_     = false;
        quint64                                    extensionAssociationId_ = 0;
        ContentKey                                 extensionAnchor_;
        QSharedPointer<ConnectionAssociationState> extensionState_;
        QList<ContentKey>                          extensionContents_;
    };

}}}

#endif // JINGLE_ICE_GROUP_P_H
