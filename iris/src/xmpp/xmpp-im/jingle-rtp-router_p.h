// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_ROUTER_P_H
#define JINGLE_RTP_ROUTER_P_H

#include "jingle-rtp-srtp.h"
#include "jingle.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <optional>

namespace XMPP::Jingle::RTP {

struct Description;

// Authenticated BUNDLE packet demultiplexing. The caller may pass packets here
// only after SRTP/SRTCP authentication. This class never decrypts packets and
// never broadcasts an ambiguous packet to multiple contents.
class BundleRouter {
public:
    static constexpr int MaxRoutes                  = 32;
    static constexpr int MaxLearnedSsrcs            = 64;
    static constexpr int MaxRegisteredOutgoingSsrcs = 64;

    struct Route {
        ContentKey    content;
        QByteArray    mid;
        quint16       midExtensionId = 0;   // 0 when MID was not negotiated for this content.
        QSet<quint8>  incomingPayloadTypes; // Allowed PTs; only globally unique PTs are fallback routes.
        QSet<quint32> incomingSsrcs;
        QSet<quint32> localSsrcs; // Peer RTCP can refer to our local media source.
    };

    enum class Delivery { Content, SharedRtcp };

    struct RoutedPacket {
        Delivery            delivery = Delivery::Content;
        ContentKey          content;
        QList<ContentKey>   relatedContents;
        QByteArray          data;
        SrtpContext::Packet kind     = SrtpContext::Packet::Rtp;
        quint64             revision = 0;
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

    // Transactional: a rejected table leaves the previous routes, runtime outgoing
    // SSRC registrations and revision intact. Registrations for surviving contents
    // are retained; registrations for removed contents are dropped on commit.
    bool configure(const QList<Route> &routes);
    void reset();

    // Packet-capable media adapters can register an SSRC as soon as it appears on
    // their outgoing RTP channel. This is required to route RR/feedback before any
    // remote RTP has established an incoming SSRC association.
    bool registerOutgoingSsrc(const ContentKey &content, quint32 ssrc);
    bool unregisterOutgoingSsrc(const ContentKey &content, quint32 ssrc);

    std::optional<RoutedPacket> routeIncoming(const QByteArray &packet, SrtpContext::Packet kind);
    bool                        isCurrent(const RoutedPacket &packet) const;

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
    static void    noteSsrc(const QHash<quint32, int> &mapping, quint32 ssrc, QSet<int> &routes);

    std::optional<ParsedRtp>    parseRtp(const QByteArray &packet) const;
    bool                        collectRtcpRoutes(const QByteArray &packet, QSet<int> &routes,
                                                  bool &unresolvedTarget) const;
    bool                        payloadAllowed(int routeIndex, quint8 payloadType) const;
    std::optional<RoutedPacket> routed(int routeIndex, const QByteArray &packet, SrtpContext::Packet kind);
    std::optional<RoutedPacket> routedSharedRtcp(const QSet<int> &routeIndexes, const QByteArray &packet);
    void                        advanceRevision();

    QList<Route>           routes_;
    QMap<ContentKey, int>  contentRoutes_;
    QHash<QByteArray, int> midRoutes_;
    // A payload type appears here only when it identifies exactly one route.
    // Colliding PTs are intentionally absent rather than guessed.
    QHash<quint8, int> payloadTypeRoutes_;
    // Incoming and local SSRCs are deliberately separate. Incoming RTP may only
    // use peer SSRCs; local SSRCs are valid only in RTCP report/media-source fields.
    QHash<quint32, int>        incomingSsrcRoutes_;
    QHash<quint32, int>        localSsrcRoutes_;
    QSet<quint32>              learnedSsrcs_;
    QHash<quint32, ContentKey> registeredOutgoingSsrcs_;
    quint16                    midExtensionId_ = 0;
    quint64                    revision_       = 0;
    Error                      lastError_      = Error::None;
};

// Convert negotiated endpoint descriptions into the static part of one router
// entry. Runtime outgoing SSRCs are registered separately when packets appear.
std::optional<BundleRouter::Route> bundleRouteForDescriptions(const ContentKey &content, bool localContent,
                                                               const Description &local,
                                                               const Description &remote);

}

#endif // JINGLE_RTP_ROUTER_P_H
