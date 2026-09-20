// Internal transactional Jingle group planning. This file has no live transport side effects.
#ifndef JINGLE_GROUP_NEGOTIATION_P_H
#define JINGLE_GROUP_NEGOTIATION_P_H

#include "jingle-session.h"

#include <QByteArray>
#include <QMap>
#include <QSet>

#include <optional>

namespace XMPP { namespace Jingle {

    struct GroupPlan {
        struct TransportParameters {
            QString    iceUfrag;
            QString    icePassword;
            QString    dtlsHash;
            QByteArray dtlsFingerprint;
            QString    dtlsSetup;

            bool operator==(const TransportParameters &other) const
            {
                return iceUfrag == other.iceUfrag && icePassword == other.icePassword && dtlsHash == other.dtlsHash
                    && dtlsFingerprint == other.dtlsFingerprint && dtlsSetup == other.dtlsSetup;
            }
            bool operator!=(const TransportParameters &other) const { return !(*this == other); }
        };

        struct Association {
            int                                id      = -1;
            bool                               bundled = false;
            ContentKey                         owner;
            QList<ContentKey>                  members;
            QString                            transportNamespace;
            std::optional<TransportParameters> transportParameters;
        };

        enum class ActionKind { CreateAssociation, AttachMember };

        struct Action {
            ActionKind kind          = ActionKind::CreateAssociation;
            int        associationId = -1;
            ContentKey content;
        };

        const QList<Association> &associations() const { return associations_; }
        const QList<Action>      &actions() const { return actions_; }
        int  associationFor(const ContentKey &content) const { return memberAssociations_.value(content, -1); }
        bool readyToCommit() const
        {
            for (const auto &association : associations_) {
                if (association.bundled && association.members.size() > 1 && !association.transportParameters)
                    return false;
            }
            return true;
        }

    private:
        friend class GroupNegotiation;
        QList<Association>    associations_;
        QList<Action>         actions_;
        QMap<ContentKey, int> memberAssociations_;
    };

    class GroupNegotiation {
    public:
        using TransportParameters = GroupPlan::TransportParameters;

        struct Member {
            ContentKey                         content;
            QString                            transportNamespace;
            bool                               shareable = false;
            std::optional<TransportParameters> transportParameters;
        };

        enum class Error {
            None,
            InvalidContent,
            DuplicateContent,
            InvalidGroup,
            UnknownContent,
            AmbiguousContent,
            InvalidAnswer,
            UnsupportedSharedTransport,
            ConflictingTransport,
            IncompleteTransportParameters,
            ConflictingTransportParameters
        };

        // A negotiated multi-content BUNDLE association is one transport
        // replacement unit. Replacing only some of its members would split one
        // live association into multiple signaling incarnations.
        static bool replacementBatchPreservesBundles(const QList<ContentGroup> &negotiated,
                                                     const QSet<ContentKey> &replacements)
        {
            QHash<QString, int> replacedNames;
            for (const auto &key : replacements)
                ++replacedNames[key.first];

            for (const auto &group : negotiated) {
                if (group.semantics != QLatin1String("BUNDLE") || group.contents.size() < 2)
                    continue;
                int present = 0;
                for (const auto &name : group.contents) {
                    const int count = replacedNames.value(name);
                    if (count > 1)
                        return false;
                    present += count ? 1 : 0;
                }
                if (present != 0 && present != group.contents.size())
                    return false;
            }
            return true;
        }

        // Build an immutable initial membership plan. This validates the whole
        // answer before producing actions and never starts ICE or mutates transports.
        // A multi-member association with no parameter snapshots is a preflight
        // plan only; readyToCommit() remains false until parameters are supplied.
        static std::optional<GroupPlan> initialPlan(const QList<Member> &members, const QList<ContentGroup> &offer,
                                                    const QList<ContentGroup> &answer, Error *error = nullptr)
        {
            auto fail = [error](Error value) -> std::optional<GroupPlan> {
                if (error)
                    *error = value;
                return std::nullopt;
            };
            if (error)
                *error = Error::None;

            QMap<ContentKey, int>     memberIndexes;
            QMap<QString, QList<int>> nameIndexes;
            for (int index = 0; index < members.size(); ++index) {
                const auto &member = members.at(index);
                if (member.content.first.isEmpty()
                    || (member.content.second != Origin::Initiator && member.content.second != Origin::Responder)
                    || member.transportNamespace.isEmpty())
                    return fail(Error::InvalidContent);
                if (memberIndexes.contains(member.content))
                    return fail(Error::DuplicateContent);
                memberIndexes.insert(member.content, index);
                nameIndexes[member.content.first].append(index);
            }

            auto resolve = [&nameIndexes](const QString &name, Error *resolutionError) -> int {
                const auto indexes = nameIndexes.value(name);
                if (indexes.isEmpty()) {
                    *resolutionError = Error::UnknownContent;
                    return -1;
                }
                if (indexes.size() != 1) {
                    *resolutionError = Error::AmbiguousContent;
                    return -1;
                }
                return indexes.first();
            };

            QMap<QString, int> offeredGroupByName;
            for (int groupIndex = 0; groupIndex < offer.size(); ++groupIndex) {
                const auto &group = offer.at(groupIndex);
                if (group.semantics != QLatin1String("BUNDLE") || group.contents.isEmpty())
                    continue;
                QSet<QString> groupNames;
                for (const auto &name : group.contents) {
                    if (name.isEmpty() || groupNames.contains(name) || offeredGroupByName.contains(name))
                        return fail(Error::InvalidGroup);
                    Error resolutionError = Error::None;
                    if (resolve(name, &resolutionError) < 0)
                        return fail(resolutionError);
                    groupNames.insert(name);
                    offeredGroupByName.insert(name, groupIndex);
                }
            }

            GroupPlan     plan;
            QSet<int>     assignedMembers;
            QSet<int>     answeredGroups;
            QSet<QString> answeredNames;

            for (const auto &group : answer) {
                if (group.semantics != QLatin1String("BUNDLE") || group.contents.isEmpty())
                    continue;
                QSet<QString> groupNames;
                const int     offeredGroup = offeredGroupByName.value(group.contents.first(), -1);
                if (offeredGroup < 0 || answeredGroups.contains(offeredGroup))
                    return fail(Error::InvalidAnswer);

                GroupPlan::Association association;
                association.id         = plan.associations_.size();
                association.bundled    = true;
                bool hasParameters     = false;
                bool missingParameters = false;

                for (const auto &name : group.contents) {
                    if (name.isEmpty() || groupNames.contains(name) || answeredNames.contains(name)
                        || offeredGroupByName.value(name, -1) != offeredGroup)
                        return fail(Error::InvalidAnswer);
                    Error     resolutionError = Error::None;
                    const int memberIndex     = resolve(name, &resolutionError);
                    if (memberIndex < 0)
                        return fail(resolutionError);
                    const auto &member = members.at(memberIndex);
                    if (group.contents.size() > 1 && !member.shareable)
                        return fail(Error::UnsupportedSharedTransport);
                    if (association.transportNamespace.isEmpty())
                        association.transportNamespace = member.transportNamespace;
                    else if (association.transportNamespace != member.transportNamespace)
                        return fail(Error::ConflictingTransport);
                    if (member.transportParameters) {
                        if (!association.transportParameters)
                            association.transportParameters = member.transportParameters;
                        else if (*association.transportParameters != *member.transportParameters)
                            return fail(Error::ConflictingTransportParameters);
                        hasParameters = true;
                    } else {
                        missingParameters = true;
                    }
                    if (association.members.isEmpty())
                        association.owner = member.content;
                    association.members.append(member.content);
                    assignedMembers.insert(memberIndex);
                    groupNames.insert(name);
                    answeredNames.insert(name);
                }
                if (hasParameters && missingParameters)
                    return fail(Error::IncompleteTransportParameters);
                answeredGroups.insert(offeredGroup);
                plan.associations_.append(association);
            }

            for (int memberIndex = 0; memberIndex < members.size(); ++memberIndex) {
                if (assignedMembers.contains(memberIndex))
                    continue;
                const auto            &member = members.at(memberIndex);
                GroupPlan::Association association;
                association.id                  = plan.associations_.size();
                association.owner               = member.content;
                association.members             = { member.content };
                association.transportNamespace  = member.transportNamespace;
                association.transportParameters = member.transportParameters;
                plan.associations_.append(association);
            }

            for (const auto &association : plan.associations_) {
                plan.actions_.append(
                    GroupPlan::Action { GroupPlan::ActionKind::CreateAssociation, association.id, association.owner });
                for (const auto &member : association.members) {
                    if (plan.memberAssociations_.contains(member))
                        return fail(Error::InvalidAnswer);
                    plan.memberAssociations_.insert(member, association.id);
                    plan.actions_.append(
                        GroupPlan::Action { GroupPlan::ActionKind::AttachMember, association.id, member });
                }
            }
            return plan;
        }
    };

}}

#endif // JINGLE_GROUP_NEGOTIATION_P_H
