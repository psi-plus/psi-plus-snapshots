// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_H
#define JINGLE_RTP_H

#include "jingle-application.h"
#include "jingle-rtp-directions.h"
#include "jingle-rtp-info.h"
#include "jingle-rtp-negotiation.h"
#include "jingle-rtp-srtp.h"
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>
#include <optional>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
Q_MOC_INCLUDE(<iris/xmpp-im/xmpp_message.h>)
#endif

namespace XMPP { class Message; }

namespace XMPP::Jingle::RTP {

enum class Media : quint8 { None = 0x00, Audio = 0x01, Video = 0x02 };
Q_DECLARE_FLAGS(MediaSet, Media)
Q_DECLARE_OPERATORS_FOR_FLAGS(MediaSet)

struct IRIS_EXPORT Proposal {
    Media media = Media::None;

    bool isValid() const { return media == Media::Audio || media == Media::Video; }
};

// Opaque backend-facing identity. Tokens have meaning only inside one Jingle
// MediaSession and are never serialized on the wire.
struct IRIS_EXPORT SecureRtpPacket {
    QByteArray           associationId;
    quint64              epoch = 0;
    QByteArray           data;
    PacketKind           kind = PacketKind::Rtp;
};

struct IRIS_EXPORT SecureRtpEndpoint {
    QByteArray      endpointId;
    QByteArray      associationId;
    QString         media;
    QByteArray      mid;
    quint16         midExtensionId = 0;
    QSet<quint8>    incomingPayloadTypes;
    QSet<quint32>   incomingSsrcs;
    QSet<quint32>   localSsrcs;

    bool isValid() const
    {
        return !endpointId.isEmpty() && !associationId.isEmpty()
            && (media == QLatin1String("audio") || media == QLatin1String("video"));
    }
};

struct IRIS_EXPORT SecureRtpParameters {
    QByteArray       associationId;
    quint64          epoch = 0;
    QString          profile;
    QCA::SecureArray localMasterKey;
    QCA::SecureArray localMasterSalt;
    QCA::SecureArray remoteMasterKey;
    QCA::SecureArray remoteMasterSalt;

    bool isValid() const { return !associationId.isEmpty() && epoch != 0 && !profile.isEmpty(); }
};

// All calls occur on the Jingle thread. Factories and negotiation must not
// capture media, start a nested event loop, or initiate network activity.
// The adapter owns its internal worker threads and must join them on destruction.
class IRIS_EXPORT MediaEndpoint : public CodecNegotiator {
public:
    virtual Description localOffer() const = 0;
    // Transitional synchronous fallback for adapters which do not implement the
    // MediaSession async hooks yet. Native media adapters must not block here.
    virtual bool configure(const Description &local, const Description &remote) = 0;
    virtual void stop() = 0; // idempotent, synchronous quiescence of callbacks
    // Parsed partial hints, not a replacement offer. Ignoring a hint is valid.
    virtual void advisory(const Description &) { }
};

struct IRIS_EXPORT MediaError {
    enum class Code { None, Unsupported, InvalidDescription, Backend, Timeout };
    Code     code = Code::None;
    QString  text;
    explicit operator bool() const { return code != Code::None; }
};

struct IRIS_EXPORT MediaOperationPolicy {
    int prepareDeadlineMs    = 15000;
    int applyDeadlineMs      = 10000;
    int maxPendingOperations = 8;

    bool isValid() const { return prepareDeadlineMs > 0 && applyDeadlineMs > 0 && maxPendingOperations > 0; }
};

class MediaSession;

// Cancellable handle for one serialized media-backend operation. Destruction is
// equivalent to cancel(). Cancellation suppresses all later completion delivery.
class IRIS_EXPORT MediaOperation {
public:
    using Id = quint64;
    ~MediaOperation();
    MediaOperation(const MediaOperation &)            = delete;
    MediaOperation &operator=(const MediaOperation &) = delete;
    Id              id() const;
    void            cancel();

private:
    class Private;
    explicit MediaOperation(Id, MediaSession *);
    std::unique_ptr<Private> d;
    friend class MediaSession;
};

// One backend media session may own several RTP endpoints (for example audio and
// video). Backend operations are serialized across all endpoints in this object.
// Public completion callbacks are always queued and therefore never run inline
// from prepareLocalOffer()/prepareAnswer()/applyNegotiation().
class IRIS_EXPORT MediaSession : public QObject {
    Q_OBJECT
public:
    using PrepareCallback = std::function<void(MediaOperation::Id, std::optional<Description>, MediaError)>;
    using ApplyCallback   = std::function<void(MediaOperation::Id, MediaError)>;

    explicit MediaSession(QObject *parent = nullptr);
    ~MediaSession() override;
    MediaSession(const MediaSession &)            = delete;
    MediaSession &operator=(const MediaSession &) = delete;

    virtual std::unique_ptr<MediaEndpoint> createEndpoint(const QString &contentName, const QString &media) = 0;

    // Optional protected group packet boundary. Per-content MediaEndpoint stays
    // responsible for codec negotiation; SRTP/SRTCP ownership belongs here so a
    // BUNDLE group shares one association while unbundled contents remain
    // independent associations inside the same backend media session.
    using ProtectedPacketWriter = std::function<bool(const SecureRtpPacket &)>;
    virtual bool configureSecureRtpEndpoints(const QList<SecureRtpEndpoint> &) { return false; }
    virtual bool configureSecureRtpAssociation(const SecureRtpParameters &) { return false; }
    virtual void invalidateSecureRtpAssociation(const QByteArray &, quint64) { }
    virtual bool receiveProtectedRtpPacket(const SecureRtpPacket &) { return false; }
    virtual bool attachSecureRtpPacketIo(ProtectedPacketWriter) { return false; }
    virtual void detachSecureRtpPacketIo() { }

    std::unique_ptr<MediaOperation> prepareLocalOffer(MediaEndpoint *, PrepareCallback);
    std::unique_ptr<MediaOperation> prepareAnswer(MediaEndpoint *, const Description &remoteSnapshot, PrepareCallback);
    std::unique_ptr<MediaOperation> applyNegotiation(MediaEndpoint *, const Description &local,
                                                     const Description &remote, ApplyCallback);

    MediaOperationPolicy operationPolicy() const;
    // Policy may only change while the serialized operation queue is idle.
    bool setOperationPolicy(const MediaOperationPolicy &);

    // Cancel queued work and the currently running backend operation. No cancelled
    // operation may subsequently deliver a callback. Pad teardown calls this while
    // the derived adapter is still alive, so cancelMediaOperation() can stop I/O.
    void cancelAll();

signals:
    // Backend failure outside a prepare/apply operation. Operation-scoped errors
    // must be returned through that operation's completion instead, never twice.
    void runtimeError(const XMPP::Jingle::RTP::MediaError &);

protected:
    using PrepareCompletion = std::function<void(std::optional<Description>, MediaError)>;
    using ApplyCompletion   = std::function<void(MediaError)>;

    // These hooks are entered one at a time on the Jingle thread. Completion must
    // also be invoked on that thread; worker-thread adapters must marshal first.
    // Preparing codecs/session state must not itself grant microphone/camera
    // capture permission. The default implementation is a migration fallback
    // around the legacy synchronous MediaEndpoint methods.
    virtual void beginPrepareLocalOffer(MediaOperation::Id, MediaEndpoint *, PrepareCompletion);
    virtual void beginPrepareAnswer(MediaOperation::Id, MediaEndpoint *, const Description &remoteSnapshot,
                                    PrepareCompletion);
    virtual void beginApplyNegotiation(MediaOperation::Id, MediaEndpoint *, const Description &local,
                                       const Description &remote, ApplyCompletion);
    virtual void cancelMediaOperation(MediaOperation::Id) { }
    // Timeout differs from caller cancellation: the live caller receives an
    // explicit Timeout error. Providers with untagged async signals may override
    // this hook to fail the whole backend session rather than reuse it.
    virtual void timeoutMediaOperation(MediaOperation::Id id) { cancelMediaOperation(id); }
    // Overridable clock seam for deterministic tests. Implementations must not
    // invoke mediaOperationDeadlineExpired() synchronously from arm().
    virtual void armMediaOperationDeadline(MediaOperation::Id, int timeoutMs);
    virtual void disarmMediaOperationDeadline(MediaOperation::Id);
    void         mediaOperationDeadlineExpired(MediaOperation::Id);

private:
    class Private;
    std::unique_ptr<Private> d;
    void                     cancelOperation(MediaOperation::Id);
    void                     scheduleNext();
    void                     startNext();
    bool                     claimCompletion(MediaOperation::Id);
    void                     finishPrepared(MediaOperation::Id, std::optional<Description>, MediaError);
    void                     finishApplied(MediaOperation::Id, MediaError);
    void                     finishTimedOut(MediaOperation::Id);
    friend class MediaOperation;
};

class IRIS_EXPORT MediaProvider {
public:
    virtual ~MediaProvider()                              = default;
    virtual std::unique_ptr<MediaSession> createSession() = 0;
    // Discovery must describe the actual backend, not merely the RTP parser.
    // Unknown providers advertise no RTP media types or packet crypto by default.
    virtual QStringList mediaTypes() const { return {}; }
    virtual QStringList secureRtpProfiles() const { return {}; }
};

class Manager;
class Application;
class IRIS_EXPORT Pad : public ApplicationManagerPad {
    Q_OBJECT
public:
    Pad(Manager *, Session *, std::shared_ptr<MediaProvider>, QStringList transports);
    ~Pad() override;
    QString              ns() const override;
    Session             *session() const override;
    ApplicationManager  *manager() const override;
    QString              generateContentName(Origin) override;
    QStringList          sessionInfoNamespaces() const override { return { SessionInfo::ns() }; }
    bool                 incomingSessionInfo(const QDomElement &) override;
    MediaSession        *mediaSession() const;
    DirectionController *directionController() const { return directions_; }
    const QStringList   &transportNamespaces() const { return transports_; }
signals:
    // Peer status only: never changes local consent or negotiated senders.
    void informationReceived(const XMPP::Jingle::RTP::SessionInfo &);
    // Call-level backend failure propagated to every still-live RTP content.
    void mediaError(const XMPP::Jingle::RTP::MediaError &);

private:
    friend class Application;
    class RoutingPrivate;
    bool bindSecureTransport(Application *, SecureRtpAssociation *, const Description &local,
                             const Description &remote);
    void unbindSecureTransport(Application *);
    bool sendProtectedPacket(const SecureRtpPacket &);
    bool configureSecureAssociation(SecureRtpAssociation *);
    bool ensureSecurePacketIo();
    QStringList secureRtpProfiles() const;

    QPointer<Manager> manager_;
    QPointer<Session> session_;
    // Provider outlives its media session; endpoints outlive neither.
    std::shared_ptr<MediaProvider> provider_;
    std::unique_ptr<MediaSession>  media_;
    QStringList                    transports_;
    quint64                        nextName_   = 0;
    DirectionController            *directions_ = nullptr; // QObject child
    bool                            securePacketIoAttached_ = false;
    std::unique_ptr<RoutingPrivate> routing_;
};

class IRIS_EXPORT Application : public XMPP::Jingle::Application {
    Q_OBJECT
public:
    Application(const QSharedPointer<Pad> &, const QString &, Origin creator, Origin senders);
    ~Application() override;
    bool                                initializeOutgoing(const QString &media);
    void                                setState(State) override;
    Update                              evaluateOutgoingUpdate() override;
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    Reason                              lastReason() const override { return reason_; }
    SetDescError                        setRemoteOffer(const QDomElement &) override;
    SetDescError                        setRemoteAnswer(const QDomElement &) override;
    QDomElement                         makeLocalOffer() override;
    QDomElement                         makeLocalAnswer() override;
    bool                                supportsContentModify() const override { return true; }
    bool                                incomingDescriptionInfo(const QDomElement &) override;
    bool                                isTransportReplaceEnabled() const override;
    bool                                allowsSharedTransport() const override { return true; }
    void                                prepare() override;
    void                                start() override;
    void                       remove(Reason::Condition = Reason::Success, const QString & = QString()) override;
    void                       incomingRemove(const Reason &) override;
    const QString             &media() const { return media_; }
    std::optional<Description> localDescription() const { return negotiation_.localDescription(); }
    std::optional<Description> remoteDescription() const { return negotiation_.remoteDescription(); }

protected:
    void prepareTransport() override;

private:
    friend class Pad;
    void                            stopMedia();
    void                            prepared(MediaOperation::Id, std::optional<Description>, MediaError);
    void                            applied(MediaOperation::Id, MediaError);
    void                            failPreparation(Reason::Condition, const QString &);
    void                            activateMedia();
    // Direction/consent policy query. This is deliberately independent of
    // packet parsing; media adapters use it to decide whether capture/transmit
    // or receive paths may be active.
    bool                            allowsRtp(bool sending) const;
    Negotiation                     negotiation_;
    std::optional<Negotiation>      beforeAnswer_;
    std::optional<Description>      pendingRemoteOffer_;
    std::unique_ptr<MediaEndpoint>  endpoint_;
    std::unique_ptr<MediaOperation> prepareOperation_;
    std::unique_ptr<MediaOperation> applyOperation_;
    QString                         media_;
    std::optional<Stanza::Error>    error_;
    Reason                          reason_;
    bool                            configured_        = false;
    bool                            secureBound_        = false;
    bool                            stopping_          = false;
    bool                            preparationFailed_ = false;
    QPointer<SecureRtpAssociation>  association_;
};

class IRIS_EXPORT Manager : public ApplicationManager {
    Q_OBJECT
public:
    explicit Manager(QObject *parent = nullptr);
    ~Manager() override;
    void setJingleManager(XMPP::Jingle::Manager *) override;
    void setMediaProvider(std::shared_ptr<MediaProvider>);
    // Explicit transport whitelist for this RTP backend. It gates both
    // transport selection and RTP discovery; an empty or unusable whitelist means
    // this manager must not advertise RTP support.
    void         setTransportNamespaces(const QStringList &);

    // Start an XEP-0353 proposal for a new RTP call. The returned UUID is also
    // the Jingle SID that must be used after <proceed/>.
    QString propose(const Jid &peer, MediaSet media);

    Application *createOutgoing(Session *, Media media, Origin senders = Origin::Both);
    Application *createOutgoing(Session *, const QString &media, Origin senders = Origin::Both);
    Application *startApplication(const ApplicationManagerPad::Ptr &, const QString &, Origin, Origin) override;
    ApplicationManagerPad *pad(Session *) override;
    void                   closeAll(const QString & = QString()) override;
    std::optional<std::any> parseProposal(const QDomElement &) const override;
    QDomElement             serializeProposal(const std::any &, QDomDocument *) const override;
    QStringList             ns() const override { return { Description::ns() }; }
    QStringList             discoFeatures() const override;
    QStringList             secureRtpProfiles() const;

signals:
    // Convenience view for ordinary RTP call proposals. Mixed/application-
    // composite JMI proposals remain available only through Jingle::Manager.
    void incomingProposal(const XMPP::Message &message, const QString &id, XMPP::Jingle::RTP::MediaSet media);

private:
    QPointer<XMPP::Jingle::Manager> jingle_;
    QMetaObject::Connection         jmiConnection_;
    std::shared_ptr<MediaProvider>  provider_;
    QStringList                     transports_;
    QList<QPointer<Application>>    applications_;
};

}
Q_DECLARE_METATYPE(XMPP::Jingle::RTP::MediaError)
Q_DECLARE_METATYPE(XMPP::Jingle::RTP::MediaSet)
#endif
