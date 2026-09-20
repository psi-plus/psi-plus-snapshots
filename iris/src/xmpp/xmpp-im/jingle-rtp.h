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

namespace XMPP::Jingle::RTP {

// All calls occur on the Jingle thread. Factories and negotiation must not
// capture media, start a nested event loop, or initiate network activity.
// The adapter owns its internal worker threads and must join them on destruction.
class IRIS_EXPORT MediaEndpoint : public CodecNegotiator {
public:
    using PacketWriter = std::function<bool(QByteArray, SrtpContext::Packet)>;
    // Stable opt-in capability; all calls, including PacketWriter, stay on the Jingle
    // thread. Worker-thread engines must use bounded queues in their adapter.
    virtual bool supportsPacketIo() const { return false; }
    // Called once after negotiated parameters are applied and authentication is
    // ready. This does not grant permission to capture media. Writer becomes
    // usable when the Application is Active. stop() must detach all callbacks.
    virtual bool        attachPacketIo(PacketWriter) { return false; }
    virtual void        receivePacket(const QByteArray &, SrtpContext::Packet) { }
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
    // Unknown providers advertise no RTP media types by default.
    virtual QStringList mediaTypes() const { return {}; }
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
    bool bindPacketRoute(Application *, SrtpSession *, const Description &local, const Description &remote);
    void unbindPacketRoute(Application *);
    bool registerOutgoingRtp(Application *, const QByteArray &);

    QPointer<Manager> manager_;
    QPointer<Session> session_;
    // Provider outlives its media session; endpoints outlive neither.
    std::shared_ptr<MediaProvider> provider_;
    std::unique_ptr<MediaSession>  media_;
    QStringList                    transports_;
    quint64                        nextName_   = 0;
    DirectionController           *directions_ = nullptr; // QObject child
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
    bool                                supportsSharedTransport() const override { return true; }
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
    bool                            sendPacket(QByteArray, SrtpContext::Packet, quint64 epoch);
    void                            receiveRoutedPacket(const QByteArray &, SrtpContext::Packet, quint64 epoch);
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
    bool                            attached_          = false;
    bool                            stopping_          = false;
    bool                            preparationFailed_ = false;
    QPointer<SrtpSession>           security_;
    QSet<int>                       negotiatedPayloads_;
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
    Application *createOutgoing(Session *, const QString &media, Origin senders = Origin::Both);
    Application *startApplication(const ApplicationManagerPad::Ptr &, const QString &, Origin, Origin) override;
    ApplicationManagerPad *pad(Session *) override;
    void                   closeAll(const QString & = QString()) override;
    QStringList            ns() const override { return { Description::ns() }; }
    QStringList            discoFeatures() const override;

private:
    QPointer<XMPP::Jingle::Manager> jingle_;
    std::shared_ptr<MediaProvider>  provider_;
    QStringList                     transports_;
    QList<QPointer<Application>>    applications_;
};

}
Q_DECLARE_METATYPE(XMPP::Jingle::RTP::MediaError)
#endif
