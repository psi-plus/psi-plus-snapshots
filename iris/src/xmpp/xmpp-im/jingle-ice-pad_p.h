// Session-local ICE state shared by the transports of one ICE pad, including BUNDLE groups.
#ifndef JINGLE_ICE_PAD_P_H
#define JINGLE_ICE_PAD_P_H

#include "jingle-ice-connection_p.h"
#include "jingle-ice-group_p.h"
#include "jingle-ice.h"

#include <QMap>
#include <QPointer>
#include <QSet>

#include <optional>

namespace XMPP { namespace Jingle { namespace ICE {

    class Pad::Private {
    public:
        Private(Pad *q, Manager *manager, Session *session);

        // Shared BUNDLE association for a content transport, or nullptr if it is not grouped.
        IceConnection *groupedConnectionFor(Transport *transport, bool *contentBound, bool *groupRequired);
        // Independent association for a content transport outside any BUNDLE group.
        ConnectionMembership membershipFor(Transport *transport, bool *contentBound);
        ConnectionMembership takeUnacceptedSingletonMembership(Transport *transport);
        bool                 groupedConnectionAccepted(Transport *transport) const;
        bool                 shouldDeferGroupedNetwork(Transport *transport) const;
        bool                 commitGroupExtension(const ContentKey &content);
        void                 rollbackGroupExtension(const ContentKey &content);
        void                 watchGroupedContent(Application *app, const ContentKey &content);
        qsizetype            liveAssociationCount() const { return registry.liveAssociationCount(); }

        Pad          *q;
        Manager      *_manager;
        Session      *_session;
        TcpPortScope *_discoScope = nullptr;

        ConnectionRegistry                        registry;
        QMap<ContentKey, QPointer<Transport>>     contentOwners;
        QSet<ContentKey>                          establishedContents;
        std::optional<ConnectionGroupTransaction> stagedGroups;
        std::optional<ConnectionGroupTransaction> extensionGroups;
        std::optional<ContentKey>                 extensionContent;
        QPointer<Transport>                       extensionTransport;
        std::optional<ConnectionGroupTransaction> replacementGroups;
        QSet<ContentKey>                          replacementContents;
        QSet<ContentKey>                          replacementBound;
        QMap<ContentKey, QPointer<Transport>>     replacementTransports;
        QList<ContentGroup>                       stagedOfferGroups;
        QSet<ContentKey>                          stagedContents;
        bool                                      groupStageAttempted  = false;
        bool                                      groupStageFailed     = false;
        bool                                      localAcceptanceKnown = false;
    };

}}}

#endif // JINGLE_ICE_PAD_P_H
