/*
 * Copyright (C) 2026 Psi IM team
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef PSIMEDIA_RTPBUNDLEROUTER_H
#define PSIMEDIA_RTPBUNDLEROUTER_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>

#include <optional>

namespace PsiMedia {

// Backend-neutral authenticated RTP demultiplexing for one RTP/BUNDLE group.
// Packets may enter this router only after SRTP authentication. RTCP is not
// routed to endpoints: it belongs to the group RTP session and is validated
// structurally with validateRtcp() before group-level ingress.
class RtpBundleRouter {
public:
    static constexpr int MaxRoutes                  = 32;
    static constexpr int MaxLearnedSsrcs            = 64;
    static constexpr int MaxRegisteredOutgoingSsrcs = 64;

    struct Route {
        QByteArray    endpointId;
        QByteArray    mid;
        quint16       midExtensionId = 0;
        QSet<quint8>  incomingPayloadTypes;
        QSet<quint32> incomingSsrcs;
        QSet<quint32> localSsrcs;
    };

    struct RoutedRtp {
        QByteArray endpointId;
        QByteArray data;
        quint64    revision = 0;
    };

    enum class Error {
        None,
        InvalidRoutes,
        MalformedPacket,
        UnknownRoute,
        AmbiguousRoute,
        DisallowedPayloadType,
        ResourceLimit
    };

    bool configure(const QList<Route> &routes);
    void reset();

    bool registerOutgoingSsrc(const QByteArray &endpointId, quint32 ssrc);
    bool unregisterOutgoingSsrc(const QByteArray &endpointId, quint32 ssrc);

    std::optional<RoutedRtp> routeIncomingRtp(const QByteArray &packet);
    bool                     validateRtcp(const QByteArray &packet) const;
    bool                     isCurrent(const RoutedRtp &packet) const;

    quint64 revision() const { return revision_; }
    Error   lastError() const { return lastError_; }
    int     learnedSsrcCount() const { return learnedSsrcs_.size(); }
    int     registeredOutgoingSsrcCount() const { return registeredOutgoingSsrcs_.size(); }

private:
    struct ParsedRtp {
        quint32                   ssrc        = 0;
        quint8                    payloadType = 0;
        std::optional<QByteArray> mid;
    };

    static quint16 read16(const QByteArray &data, int offset);
    static quint32 read32(const QByteArray &data, int offset);

    std::optional<ParsedRtp> parseRtp(const QByteArray &packet) const;
    bool                     payloadAllowed(int routeIndex, quint8 payloadType) const;
    std::optional<RoutedRtp> routed(int routeIndex, const QByteArray &packet);
    void                     advanceRevision();

    QList<Route>               routes_;
    QHash<QByteArray, int>     endpointRoutes_;
    QHash<QByteArray, int>     midRoutes_;
    QHash<quint8, int>         payloadTypeRoutes_;
    QHash<quint32, int>        incomingSsrcRoutes_;
    QHash<quint32, int>        localSsrcRoutes_;
    QSet<quint32>              learnedSsrcs_;
    QHash<quint32, QByteArray> registeredOutgoingSsrcs_;
    quint16                    midExtensionId_ = 0;
    quint64                    revision_       = 0;
    mutable Error              lastError_      = Error::None;
};

} // namespace PsiMedia

#endif // PSIMEDIA_RTPBUNDLEROUTER_H
