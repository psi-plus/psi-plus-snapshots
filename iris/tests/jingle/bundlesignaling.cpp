// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/jingle-ice-connection_p.h"
#include <QChildEvent>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <qca.h>

#define private public
#include <iris/jingle-ice.h>
#include <iris/jingle-session.h>
#undef private

#include <iris/jingle-ft.h>
#include <iris/jingle-rtp.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Ack final : public Task {
public:
    explicit Ack(Task *parent) : Task(parent) { setSuccess(); }
};

class Endpoint final : public J::RTP::MediaEndpoint {
public:
    explicit Endpoint(QString media, QString content = {}, std::shared_ptr<QSet<QString>> configured = {}) :
        media_(std::move(media)), content_(std::move(content)), configured_(std::move(configured))
    {
    }

    J::RTP::Description localOffer() const override
    {
        J::RTP::Description description;
        description.media   = media_;
        description.rtcpMux = true;
        description.ssrc    = media_ == QLatin1String("audio") ? 0x11111111u : 0x22222222u;
        J::RTP::PayloadType payload;
        payload.id        = media_ == QLatin1String("audio") ? 96 : 97;
        payload.name      = media_ == QLatin1String("audio") ? QStringLiteral("opus") : QStringLiteral("VP8");
        payload.clockrate = media_ == QLatin1String("audio") ? 48000u : 90000u;
        if (media_ == QLatin1String("audio"))
            payload.channels = 2;
        description.payloads.append(payload);
        return description;
    }

    std::optional<J::RTP::Description> makeAnswer(const J::RTP::Description &offer) const override
    {
        if (offer.media != media_ || !offer.rtcpMux || offer.payloads.isEmpty())
            return {};
        auto answer = offer;
        // Distinguish responder SSRCs while preserving offered PT identifiers.
        answer.ssrc = offer.ssrc.value_or(0x22222222u) ^ 0x66666666u;
        return answer;
    }

    bool acceptsAnswer(const J::RTP::Description &, const J::RTP::Description &) const override { return true; }
    bool configure(const J::RTP::Description &, const J::RTP::Description &) override
    {
        if (configured_)
            configured_->insert(content_);
        return true;
    }
    void stop() override { }

private:
    QString                        media_;
    QString                        content_;
    std::shared_ptr<QSet<QString>> configured_;
};

class MediaSession final : public J::RTP::MediaSession {
public:
    explicit MediaSession(std::shared_ptr<QSet<QString>> configured = {}) : configured_(std::move(configured)) { }
    std::unique_ptr<J::RTP::MediaEndpoint> createEndpoint(const QString &content, const QString &media) override
    {
        if (media != QLatin1String("audio") && media != QLatin1String("video"))
            return {};
        return std::make_unique<Endpoint>(media, content, configured_);
    }

    bool attachSecureRtpPacketIo(ProtectedPacketWriter writer) override
    {
        writer_ = std::move(writer);
        return true;
    }
    void detachSecureRtpPacketIo() override { writer_ = {}; }
    bool configureSecureRtpEndpoints(const QList<J::RTP::SecureRtpEndpoint> &) override { return true; }
    bool configureSecureRtpAssociation(const J::RTP::SecureRtpParameters &parameters) override
    {
        return parameters.isValid();
    }
    void invalidateSecureRtpAssociation(const QByteArray &, quint64) override { }
    bool receiveProtectedRtpPacket(const J::RTP::SecureRtpPacket &) override { return true; }

private:
    ProtectedPacketWriter          writer_;
    std::shared_ptr<QSet<QString>> configured_;
};

class Provider final : public J::RTP::MediaProvider {
public:
    explicit Provider(std::shared_ptr<QSet<QString>> configured = {}) : configured_(std::move(configured)) { }
    std::unique_ptr<J::RTP::MediaSession> createSession() override
    {
        return std::make_unique<MediaSession>(configured_);
    }
    QStringList mediaTypes() const override { return { QStringLiteral("audio"), QStringLiteral("video") }; }
    QStringList secureRtpProfiles() const override { return { QStringLiteral("SRTP_AES128_CM_HMAC_SHA1_80") }; }

private:
    std::shared_ptr<QSet<QString>> configured_;
};

static void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-bundle-signaling"));
    disco.setFeatures(Features(features));

    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

static QStringList rtpIcePeerFeatures(Client &client, J::RTP::Manager *rtp)
{
    const auto features = client.jingleManager()->discoFeatures();
    check(features.contains(J::RTP::Description::ns()), "production caps omitted RTP description support");
    check(features.contains(J::ICE::NS), "production caps omitted ICE support");
    check(features.contains(QStringLiteral("urn:ietf:rfc:5888")),
          "production caps omitted grouping support on the feature branch");
    Q_UNUSED(rtp);
    return features;
}

static bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return condition();
}

struct WireOffer {
    QDomDocument doc;
    QDomElement  root;
    QString      audioName;
    QString      videoName;
};

static WireOffer makeOffer(Client &client, TcpPortReserver *reserver, J::RTP::MediaSet media,
                           const QString &transportNamespace = J::ICE::NS)
{
    check(media != J::RTP::MediaSet {}, "wire offer needs at least one media content");
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces(transportNamespace == J::ICE::NS ? QStringList { J::ICE::NS_ICE_UDP, J::ICE::NS }
                                                                 : QStringList { transportNamespace });

    const Jid peer(QStringLiteral("responder@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session                   session(client.jingleManager(), peer, J::Origin::Initiator);
    WireOffer                    offer;
    QList<J::RTP::Application *> applications;
    QStringList                  members;
    for (const auto type : { J::RTP::Media::Audio, J::RTP::Media::Video }) {
        if (!media.testFlag(type))
            continue;
        auto application = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, type, J::Origin::Both));
        check(application, "failed to create outgoing RTP application");
        applications.append(application);
        members.append(application->contentName());
        if (type == J::RTP::Media::Audio)
            offer.audioName = application->contentName();
        else
            offer.videoName = application->contentName();
    }

    for (auto application : applications)
        application->prepare();
    check(session.groupings().size() == 1 && session.groupings().first().semantics == QLatin1String("BUNDLE")
              && session.groupings().first().contents == members,
          "caps-driven initiator did not automatically propose RTP BUNDLE");

    QList<QSharedPointer<J::ICE::Transport>> transports;
    for (auto application : applications) {
        auto transport = qSharedPointerDynamicCast<J::ICE::Transport>(application->transport());
        check(bool(transport), "caps-driven RTP selection did not choose ICE");
        check(transport->pad()->ns() == transportNamespace, "RTP BUNDLE selected a lower-priority ICE profile");
        transports.append(transport);
    }
    auto icePad = transports.first()->pad().staticCast<J::ICE::Pad>();
    check(waitFor([&]() {
              for (qsizetype i = 0; i < applications.size(); ++i) {
                  if (!applications.at(i)->localDescription() || !transports.at(i)->hasUpdates())
                      return false;
              }
              return true;
          }),
          "outgoing BUNDLE offer did not finish RTP/ICE preparation");
    check(icePad->liveAssociationCount() == 1, "initiator BUNDLE offer did not stage one association");

    J::Jingle jingle(J::Action::SessionInitiate, QStringLiteral("bundle-signaling-test"));
    jingle.setInitiator(Jid(QStringLiteral("initiator@example.test/device")));
    offer.root = jingle.toXml(&offer.doc);
    for (qsizetype i = 0; i < applications.size(); ++i) {
        auto application                 = applications.at(i);
        auto [transportXml, acknowledge] = transports.at(i)->takeOutgoingUpdate(false);
        check(!transportXml.isNull(), "missing BUNDLE ICE offer update");
        // Complete the local IQ boundary so fingerprint state is not left
        // half-sent while this temporary initiator Session is destroyed.
        if (acknowledge) {
            Ack result(client.rootTask());
            acknowledge(&result);
        }
        J::ContentBase content(J::Origin::Initiator, application->contentName());
        content.senders = J::Origin::Both;
        auto contentXml = content.toXml(&offer.doc, QStringLiteral("content"), J::NS);
        check(contentXml.namespaceURI() == J::NS, "wire offer content lost the Jingle namespace");
        contentXml.appendChild(offer.doc.importNode(application->makeLocalOffer(), true));
        contentXml.appendChild(offer.doc.importNode(transportXml, true));
        offer.root.appendChild(contentXml);
    }

    auto group = offer.doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &name : members) {
        auto member
            = offer.doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), name);
        group.appendChild(member);
    }
    offer.root.appendChild(group);
    return offer;
}

static QDomElement sourceContent(const WireOffer &offer, const QString &name)
{
    for (auto content = offer.root.firstChildElement(QStringLiteral("content")); !content.isNull();
         content      = content.nextSiblingElement(QStringLiteral("content"))) {
        if (content.attribute(QStringLiteral("name")) == name)
            return content;
    }
    return {};
}

static QDomElement replacementPayload(QDomDocument &doc, const WireOffer &offer,
                                      const QList<QPair<QString, QString>> &contents)
{
    auto root = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    doc.appendChild(root);

    for (const auto &[name, sourceName] : contents) {
        const auto source = sourceContent(offer, sourceName);
        check(!source.isNull(), "replacement fixture could not find offered content");
        auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
        check(!sourceTransport.isNull(), "replacement fixture could not find offered ICE transport");

        J::ContentBase cb(J::Origin::Initiator, name);
        cb.senders     = J::Origin::Both;
        auto content   = cb.toXml(&doc, QStringLiteral("content"), J::NS);
        auto transport = doc.importNode(sourceTransport, true).toElement();
        transport.setAttribute(QStringLiteral("ufrag"), QStringLiteral("bundle-restart-ufrag"));
        transport.setAttribute(QStringLiteral("pwd"), QStringLiteral("bundle-restart-password"));
        content.appendChild(transport);
        root.appendChild(content);
    }
    return root;
}

class RootTaskKeeper final : public QObject {
public:
    explicit RootTaskKeeper(Task *root) : root_(root)
    {
        check(root_, "missing client root task");
        root_->installEventFilter(this);
    }

    ~RootTaskKeeper() override
    {
        if (root_)
            root_->removeEventFilter(this);
        release();
    }

    QList<Task *> tasks()
    {
        QList<Task *> result;
        for (const auto &candidate : std::as_const(candidates_)) {
            auto task = qobject_cast<Task *>(candidate.data());
            if (!isJingleTask(task) || result.contains(task))
                continue;
            result.append(task);
            if (!held_.contains(task))
                held_.append(task);
        }
        return result;
    }

    Task *task()
    {
        const auto pending = tasks();
        return pending.isEmpty() ? nullptr : pending.constFirst();
    }

    void release()
    {
        const auto held = held_;
        held_.clear();
        for (const auto &task : held) {
            if (!task)
                continue;
            task->removeEventFilter(this);
            task->deleteLater();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == root_ && event->type() == QEvent::ChildAdded) {
            auto child = static_cast<QChildEvent *>(event)->child();
            if (child) {
                candidates_.append(child);
                child->installEventFilter(this);
            }
        } else if (watched != root_ && event->type() == QEvent::DeferredDelete) {
            auto task = qobject_cast<Task *>(watched);
            if (isJingleTask(task)) {
                if (!held_.contains(task))
                    held_.append(task);
                // With no real XMPP stream, Task::go(true) immediately
                // schedules deletion. A single logical Jingle step may
                // emit more than one IQ (for example transport-info next
                // to content-accept), so retain every Jingle task until
                // the fixture has acknowledged the whole wire boundary.
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    static bool isJingleTask(Task *task)
    {
        return task && QLatin1String(task->metaObject()->className()) == QLatin1String("XMPP::Jingle::JT");
    }

    QPointer<Task>           root_;
    QList<QPointer<QObject>> candidates_;
    QList<QPointer<Task>>    held_;
};

static void acknowledgeJingleTask(RootTaskKeeper &keeper, const Jid &peer, bool success = true)
{
    const auto pending = keeper.tasks();
    check(!pending.isEmpty(), "outgoing Jingle action did not create a retained Jingle IQ task");

    for (auto task : pending) {
        if (!task)
            continue;
        QDomDocument replyDoc;
        auto         reply = replyDoc.createElement(QStringLiteral("iq"));
        replyDoc.appendChild(reply);
        reply.setAttribute(QStringLiteral("type"), success ? QStringLiteral("result") : QStringLiteral("error"));
        if (!success) {
            Stanza::Error error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::NotAcceptable);
            reply.appendChild(error.toXml(replyDoc, QStringLiteral("jabber:client")));
        }
        reply.setAttribute(QStringLiteral("from"), peer.full());
        reply.setAttribute(QStringLiteral("id"), task->id());
        check(task->take(reply), "outgoing Jingle IQ result was not consumed");
    }
    keeper.release();
}

static void exerciseScreenAnswer(const WireOffer &offer, TcpPortReserver *reserver, const QString &scenario,
                                 bool selectedRole = true)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto configured = std::make_shared<QSet<QString>>();
    auto rtp        = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>(configured));
    const auto anchorName = offer.audioName.isEmpty() ? offer.videoName : offer.audioName;
    rtp->setTransportNamespaces(
        { sourceContent(offer, anchorName).firstChildElement(QStringLiteral("transport")).namespaceURI() });
    const Jid peer(QStringLiteral("initiator@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Responder);
    check(session.incomingInitiate(J::Jingle(offer.root), offer.root), "screen fixture rejected initial offer");
    auto anchor = session.content(anchorName, J::Origin::Initiator);
    check(anchor, "screen fixture lost its initial content");
    auto           anchorTransport = anchor->transport().staticCast<J::ICE::Transport>();
    auto           icePad          = anchorTransport->pad().staticCast<J::ICE::Pad>();
    RootTaskKeeper initialAnswer(client.rootTask());
    session.accept();
    check(waitFor([&]() { return initialAnswer.task() != nullptr; }), "initial answer was not serialized");
    acknowledgeJingleTask(initialAnswer, peer);
    check(waitFor([&]() { return configured->contains(anchorName); }), "initial media did not start on ACK");
    const auto originalGroups = session.negotiatedGroupings();
    check(session.state() == J::State::Active && originalGroups.size() == 1
              && originalGroups.first().contents.size()
                  == (int(!offer.audioName.isEmpty()) + int(!offer.videoName.isEmpty())),
          "initial audio/video BUNDLE was not negotiated");
    bool  bound = false, required = false;
    auto *network = icePad->groupedConnectionFor(anchorTransport.data(), &bound, &required);
    check(network && bound && required && icePad->liveAssociationCount() == 1,
          "initial BUNDLE did not retain its association");
    const auto initialGeneration = network->generation;

    const QString screenName = QStringLiteral("screen");
    QDomDocument  addDoc;
    auto          add = J::Jingle(J::Action::ContentAdd, session.sid()).toXml(&addDoc);
    addDoc.appendChild(add);
    J::ContentBase cb(J::Origin::Initiator, screenName);
    cb.senders       = J::Origin::Initiator;
    auto content     = cb.toXml(&addDoc, QStringLiteral("content"), J::NS);
    auto description = Endpoint(QStringLiteral("video")).localOffer();
    description.ssrc = 0x55555555u;
    content.appendChild(description.toXml(addDoc));
    auto extensionTransport
        = addDoc.importNode(sourceContent(offer, anchorName).firstChildElement(QStringLiteral("transport")), true)
              .toElement();
    // The initial offer used actpass. Its answer selected an active receiver,
    // so the offerer's established DTLS role in this content-add is passive.
    if (selectedRole)
        extensionTransport.firstChildElement(QStringLiteral("fingerprint"))
            .setAttribute(QStringLiteral("setup"), QStringLiteral("passive"));
    // Resolving actpass must not permit changing the selected role or identity.
    auto invalidTransport   = extensionTransport.cloneNode(true).toElement();
    auto invalidFingerprint = invalidTransport.firstChildElement(QStringLiteral("fingerprint"));
    invalidFingerprint.setAttribute(QStringLiteral("setup"), QStringLiteral("active"));
    check(anchorTransport->prepareUpdate(invalidTransport).status == J::Transport::PrepareUpdateStatus::Invalid,
          "BUNDLE follower changed the negotiated DTLS role");
    invalidFingerprint.setAttribute(QStringLiteral("setup"), QStringLiteral("passive"));
    auto fingerprintText = invalidFingerprint.text();
    check(!fingerprintText.isEmpty(), "screen fixture has no certificate fingerprint");
    fingerprintText[0] = fingerprintText[0] == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
    invalidFingerprint.firstChild().setNodeValue(fingerprintText);
    check(anchorTransport->prepareUpdate(invalidTransport).status == J::Transport::PrepareUpdateStatus::Invalid
              && network->generation == initialGeneration,
          "BUNDLE follower changed certificate identity or runtime generation");
    content.appendChild(extensionTransport);
    add.appendChild(content);
    auto members = originalGroups.first().contents;
    members.append(screenName);
    auto group = addDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &name : members) {
        auto member = addDoc.createElementNS(group.namespaceURI(), QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), name);
        group.appendChild(member);
    }
    add.appendChild(group);
    check(session.updateFromXml(J::Action::ContentAdd, add), "screen BUNDLE extension was rejected");
    auto screen = session.content(screenName, J::Origin::Initiator);
    check(screen, "screen addition did not create an application");
    auto           screenTransport = screen->transport().staticCast<J::ICE::Transport>();
    RootTaskKeeper answer(client.rootTask());
    screen->prepare();
    check(waitFor([&]() { return answer.task() != nullptr; }), "screen answer was not serialized");
    bool screenBound = false, screenRequired = false;
    check(icePad->groupedConnectionFor(screenTransport.data(), &screenBound, &screenRequired) == network && screenBound
              && screenRequired && icePad->liveAssociationCount() == 1 && network->generation == initialGeneration
              && !configured->contains(screenName)
              && session.negotiatedGroupings().first().contents == originalGroups.first().contents,
          "screen preparation changed committed BUNDLE or started media before answer ACK");

    if (scenario == QLatin1String("destroy"))
        delete screen;
    else if (scenario == QLatin1String("remove"))
        screen->remove(J::Reason::Cancel);
    acknowledgeJingleTask(answer, peer, scenario != QLatin1String("error"));
    if (scenario == QLatin1String("success")) {
        check(waitFor([&]() { return configured->contains(screenName); }), "screen answer ACK did not start media");
        check(session.negotiatedGroupings().first().contents == members
                  && screenTransport->rtpAssociation() == anchorTransport->rtpAssociation()
                  && icePad->liveAssociationCount() == 1
                  && network->generation.iceGeneration == initialGeneration.iceGeneration
                  && network->generation.dtlsEpoch == initialGeneration.dtlsEpoch,
              "screen acceptance allocated or restarted the established association");
    } else {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        check(!configured->contains(screenName) && !screenTransport->rtpAssociation()
                  && session.state() == J::State::Active
                  && session.negotiatedGroupings().first().contents == originalGroups.first().contents
                  && icePad->liveAssociationCount() == 1 && network->generation == initialGeneration,
              "failed or stale screen answer started media or changed established BUNDLE");
    }
}

#ifdef IRIS_TEST_SCTP
static void exerciseMixedRtpFileTransferBundle(TcpPortReserver *reserver)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("mixed@example.test/device"));
    auto      features = rtpIcePeerFeatures(client, rtp);
    // Force the data-oriented application onto the exact same ICE namespace
    // as RTP so this fixture tests one physical ICE/DTLS association rather
    // than transport preference ordering.
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1"));
    setPeerFeatures(client, peer, features);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto       audio
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto video
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(audio && video, "mixed BUNDLE fixture could not create RTP applications");

    std::unique_ptr<J::Application> ftOwner(session.newContent(J::FileTransfer::NS, J::Origin::Initiator));
    auto                            ft = dynamic_cast<J::FileTransfer::Application *>(ftOwner.get());
    check(ft, "mixed BUNDLE fixture could not create file-transfer application");
    J::FileTransfer::File file;
    file.setName(QStringLiteral("frame-metadata.bin"));
    file.setSize(32);
    ft->setFile(file);
    auto ftContent = ftOwner.release();
    session.addContent(ftContent);

    RootTaskKeeper pendingInitiate(client.rootTask());
    session.initiate();

    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    auto ftTransport    = qSharedPointerDynamicCast<J::ICE::Transport>(ft->transport());
    check(audioTransport && videoTransport && ftTransport,
          "mixed BUNDLE initial transport preselection did not choose ICE for every content");

    const auto groups = session.groupings();
    check(groups.size() == 1 && groups.first().semantics == QLatin1String("BUNDLE")
              && groups.first().contents.size() == 3 && groups.first().contents.contains(audio->contentName())
              && groups.first().contents.contains(video->contentName())
              && groups.first().contents.contains(ft->contentName()),
          "automatic grouping did not include compatible RTP and file-transfer contents");

    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();
    check(waitFor([&]() {
              return audioTransport->state() >= J::State::ApprovedToSend
                  && videoTransport->state() >= J::State::ApprovedToSend
                  && ftTransport->state() >= J::State::ApprovedToSend && audioTransport->rtpAssociation()
                  && videoTransport->rtpAssociation() && ftTransport->rtpAssociation()
                  && icePad->liveAssociationCount() == 1;
          }),
          "mixed RTP/SCTP BUNDLE did not prepare one shared secure association");

    bool  audioBound = false, audioRequired = false;
    bool  videoBound = false, videoRequired = false;
    bool  ftBound = false, ftRequired = false;
    auto *audioNetwork = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    auto *videoNetwork = icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired);
    auto *ftNetwork    = icePad->groupedConnectionFor(ftTransport.data(), &ftBound, &ftRequired);
    check(audioBound && videoBound && ftBound && audioRequired && videoRequired && ftRequired && audioNetwork
              && audioNetwork == videoNetwork && audioNetwork == ftNetwork,
          "mixed RTP/SCTP BUNDLE members did not bind to one ICE connection");
    check(audioTransport->rtpAssociation() == videoTransport->rtpAssociation()
              && audioTransport->rtpAssociation() == ftTransport->rtpAssociation(),
          "mixed RTP/SCTP BUNDLE did not reuse the one DTLS/SRTP association");
}
#endif

static QDomElement sessionAcceptPayload(QDomDocument &doc, const J::Session &session, const WireOffer &transportSource,
                                        J::RTP::Application *audio, J::RTP::Application *video, const Jid &peer)
{
    J::Jingle jingle(J::Action::SessionAccept, session.sid());
    jingle.setResponder(peer);
    auto root = jingle.toXml(&doc);
    doc.appendChild(root);

    auto appendAnswer = [&](J::RTP::Application *application, const QString &sourceName, quint32 ssrc) {
        const auto local = application->localDescription();
        check(local.has_value(), "initiator RTP offer disappeared before session-accept");

        auto answer = *local;
        answer.ssrc = ssrc;

        const auto source = sourceContent(transportSource, sourceName);
        check(!source.isNull(), "session-accept fixture could not find source ICE content");
        auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
        check(!sourceTransport.isNull(), "session-accept fixture could not find source ICE transport");

        J::ContentBase cb(J::Origin::Initiator, application->contentName());
        cb.senders   = J::Origin::Both;
        auto content = cb.toXml(&doc, QStringLiteral("content"), J::NS);
        content.appendChild(answer.toXml(doc));

        auto transport = doc.importNode(sourceTransport, true).toElement();
        transport.setAttribute(QStringLiteral("ufrag"), QStringLiteral("bundle-answer-ufrag"));
        transport.setAttribute(QStringLiteral("pwd"), QStringLiteral("bundle-answer-password"));
        auto fingerprint = transport.firstChildElement(QStringLiteral("fingerprint"));
        if (!fingerprint.isNull())
            fingerprint.setAttribute(QStringLiteral("setup"), QStringLiteral("passive"));
        content.appendChild(transport);
        root.appendChild(content);
    };

    appendAnswer(audio, transportSource.audioName, 0x33333333u);
    if (video)
        appendAnswer(video, transportSource.videoName, 0x44444444u);

    auto group = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (auto application : { audio, video }) {
        if (!application)
            continue;
        auto member = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), application->contentName());
        group.appendChild(member);
    }
    root.appendChild(group);
    return root;
}

static void exerciseInitialSingletonAnswer(const WireOffer &transportSource, TcpPortReserver *reserver, bool bundled)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto configured = std::make_shared<QSet<QString>>();
    auto rtp        = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>(configured));
    rtp->setTransportNamespaces({ J::ICE::NS });
    const Jid peer(QStringLiteral("responder@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));
    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto       audio
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    check(audio, "singleton fixture did not create audio");
    RootTaskKeeper initiate(client.rootTask());
    session.initiate();
    check(waitFor([&]() { return initiate.task() != nullptr; }), "singleton initiate was not serialized");
    acknowledgeJingleTask(initiate, peer);
    auto  audioTransport = audio->transport().staticCast<J::ICE::Transport>();
    auto  icePad         = audioTransport->pad().staticCast<J::ICE::Pad>();
    bool  bound = false, required = false;
    auto *network = icePad->groupedConnectionFor(audioTransport.data(), &bound, &required);
    check(network && bound && required && session.groupings().first().contents == QStringList { audio->contentName() },
          "audio-only initiate did not propose singleton BUNDLE");
    const auto   initialGeneration = network->generation;
    const auto   initialSecurity   = audioTransport->rtpAssociation();
    QDomDocument answerDoc;
    auto         answer = sessionAcceptPayload(answerDoc, session, transportSource, audio, nullptr, peer);
    if (!bundled)
        answer.removeChild(answer.firstChildElement(QStringLiteral("group")));
    check(session.updateFromXml(J::Action::SessionAccept, answer), "singleton answer was rejected");
    check(waitFor([&]() { return configured->contains(audio->contentName()); }),
          "singleton answer did not start audio");
    check(audio->state() == J::State::Connecting && audioTransport->state() == J::State::Connecting
              && audioTransport->rtpAssociation() == initialSecurity && icePad->liveAssociationCount() == 1
              && network->generation == initialGeneration && session.negotiatedGroupings().isEmpty() == !bundled,
          "initial singleton acceptance/refusal restarted or failed the audio transport");

    RootTaskKeeper addition(client.rootTask());
    auto           screen = dynamic_cast<J::RTP::Application *>(
        rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Initiator));
    check(screen, "singleton call did not allow a video addition");
    screen->prepare();
    check(waitFor([&]() { return addition.task() != nullptr; }), "video addition was not serialized");
    auto  screenTransport = screen->transport().staticCast<J::ICE::Transport>();
    bool  screenBound = false, screenRequired = false;
    auto *screenNetwork = icePad->groupedConnectionFor(screenTransport.data(), &screenBound, &screenRequired);
    check(screenBound && screenRequired == bundled && (bundled ? screenNetwork == network : !screenNetwork)
              && (screenTransport->rtpAssociation() == initialSecurity) == bundled
              && icePad->liveAssociationCount() == (bundled ? 1 : 2) && !configured->contains(screen->contentName())
              && network->generation == initialGeneration,
          "video extension ignored the peer's initial grouping decision");
    acknowledgeJingleTask(addition, peer);
    check(screen->state() == J::State::Pending && !configured->contains(screen->contentName()),
          "content-add receipt ACK accepted or started video");
}

static void exerciseResponder(const WireOffer &offer, TcpPortReserver *reserver, bool acceptBundle)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("initiator@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Responder);
    J::Jingle  parsed(offer.root);
    check(parsed.isValid() && parsed.action() == J::Action::SessionInitiate, "wire BUNDLE offer did not parse");
    check(session.incomingInitiate(parsed, offer.root), "responder rejected BUNDLE session-initiate");

    check(session.remoteGroupings().size() == 1
              && session.remoteGroupings().first().semantics == QLatin1String("BUNDLE")
              && session.remoteGroupings().first().contents == QStringList({ offer.audioName, offer.videoName }),
          "responder lost offered BUNDLE membership");

    auto audio = dynamic_cast<J::RTP::Application *>(session.content(offer.audioName, J::Origin::Initiator));
    auto video = dynamic_cast<J::RTP::Application *>(session.content(offer.videoName, J::Origin::Initiator));
    check(audio && video, "responder did not create production RTP applications");

    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "responder did not create production ICE transports");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    // Incoming transport updates are committed asynchronously. Before local
    // consent they must remain pure per-content signaling state and allocate no
    // physical association.
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    check(icePad->liveAssociationCount() == 0, "responder allocated BUNDLE association before local grouping decision");

    if (acceptBundle) {
        check(session.groupings().size() == 1 && session.groupings().first().semantics == QLatin1String("BUNDLE")
                  && session.groupings().first().contents == QStringList({ offer.audioName, offer.videoName }),
              "caps-driven responder did not automatically accept compatible BUNDLE");
    } else {
        session.setAutomaticGroupingEnabled(false);
        check(session.groupings().isEmpty(), "disabling automatic grouping retained responder BUNDLE");
    }

    session.accept();

    const qsizetype expectedAssociations = acceptBundle ? 1 : 2;
    check(waitFor([&]() {
              return audio->state() >= J::State::ApprovedToSend && video->state() >= J::State::ApprovedToSend
                  && audio->state() < J::State::Finishing && video->state() < J::State::Finishing
                  && icePad->liveAssociationCount() == expectedAssociations;
          }),
          acceptBundle ? "accepted BUNDLE did not allocate one live shared association"
                       : "BUNDLE refusal did not allocate live independent associations");
    check(audio->state() < J::State::Finishing && video->state() < J::State::Finishing,
          "responder RTP application failed while preparing accepted transports");

    bool  audioBound = false, audioRequired = false, videoBound = false, videoRequired = false;
    auto *audioNetwork = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    auto *videoNetwork = icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired);
    check(audioBound && videoBound, "responder transports lost content identity");

    if (acceptBundle) {
        check(audioRequired && videoRequired && audioNetwork && audioNetwork == videoNetwork,
              "accepted BUNDLE did not bind both contents to one staged association");
        check(audioTransport->rtpAssociation() && audioTransport->rtpAssociation() == videoTransport->rtpAssociation(),
              "accepted BUNDLE did not share responder SRTP");

    } else {
        check(!audioRequired && !videoRequired && !audioNetwork && !videoNetwork,
              "BUNDLE refusal retained staged shared membership");
        check(audioTransport->rtpAssociation() && videoTransport->rtpAssociation()
                  && audioTransport->rtpAssociation() != videoTransport->rtpAssociation(),
              "BUNDLE refusal did not retain independent SRTP associations");
    }
}

#ifdef IRIS_TEST_SCTP
static QDomElement activeFileAddPayload(QDomDocument &doc, const J::Session &session, const WireOffer &transportSource,
                                        const QString &name, const QStringList &bundleMembers)
{
    J::Jingle jingle(J::Action::ContentAdd, session.sid());
    auto      root = jingle.toXml(&doc);
    doc.appendChild(root);

    J::ContentBase cb(J::Origin::Initiator, name);
    cb.senders   = J::Origin::Initiator;
    auto content = cb.toXml(&doc, QStringLiteral("content"), J::NS);

    J::FileTransfer::File file;
    file.setName(name + QStringLiteral(".bin"));
    file.setSize(32);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("active-bundle-extension")));
    auto description = doc.createElementNS(J::FileTransfer::NS, QStringLiteral("description"));
    description.appendChild(file.toXml(&doc));
    content.appendChild(description);

    const auto source = sourceContent(transportSource, transportSource.audioName);
    check(!source.isNull(), "active extension fixture could not find source content");
    const auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
    check(!sourceTransport.isNull(), "active extension fixture could not find source transport");
    content.appendChild(doc.importNode(sourceTransport, true));
    root.appendChild(content);

    auto group = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
    group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &memberName : bundleMembers) {
        auto member = doc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), memberName);
        group.appendChild(member);
    }
    root.appendChild(group);
    return root;
}

static void exerciseActiveFileTransferBundleExtension(const WireOffer &offer, TcpPortReserver *reserver,
                                                      const QString &scenario = {})
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("initiator@example.test/device"));
    auto      features = rtpIcePeerFeatures(client, rtp);
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1"));
    setPeerFeatures(client, peer, features);

    J::Session session(client.jingleManager(), peer, J::Origin::Responder);
    J::Jingle  parsed(offer.root);
    check(parsed.isValid() && session.incomingInitiate(parsed, offer.root),
          "active extension responder rejected initial BUNDLE");

    auto audio = dynamic_cast<J::RTP::Application *>(session.content(offer.audioName, J::Origin::Initiator));
    auto video = dynamic_cast<J::RTP::Application *>(session.content(offer.videoName, J::Origin::Initiator));
    check(audio && video, "active extension fixture lost initial RTP applications");
    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "active extension fixture lost initial ICE transports");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    RootTaskKeeper pendingInitialAccept(client.rootTask());
    session.accept();
    check(waitFor([&]() { return pendingInitialAccept.task() != nullptr; }),
          "responder did not serialize initial session-accept");
    acknowledgeJingleTask(pendingInitialAccept, peer);
    check(waitFor([&]() { return session.state() == J::State::Active; }),
          "responder did not become Active after session-accept acknowledgement");
    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents == QStringList({ offer.audioName, offer.videoName })
              && icePad->liveAssociationCount() == 1,
          "active extension fixture did not establish initial BUNDLE");

    bool  audioBound = false, audioRequired = false;
    bool  videoBound = false, videoRequired = false;
    auto *shared = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    check(shared && audioBound && audioRequired
              && icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired) == shared
              && videoBound && videoRequired,
          "initial active RTP contents did not share one association");

    const QString fileName = QStringLiteral("active-file");
    QDomDocument  addDoc;
    auto add = activeFileAddPayload(addDoc, session, offer, fileName, { offer.audioName, offer.videoName, fileName });
    check(session.updateFromXml(J::Action::ContentAdd, add),
          "valid active BUNDLE file-transfer extension was rejected");
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    auto ft = dynamic_cast<J::FileTransfer::Application *>(session.content(fileName, J::Origin::Initiator));
    check(ft, "active BUNDLE extension did not create file-transfer application");
    auto ftTransport = qSharedPointerDynamicCast<J::ICE::Transport>(ft->transport());
    check(bool(ftTransport), "active BUNDLE extension did not create ICE file-transfer transport");
    check(session.negotiatedGroupings().first().contents == QStringList({ offer.audioName, offer.videoName }),
          "incoming content-add published BUNDLE membership before content-accept");

    RootTaskKeeper pendingContentAccept(client.rootTask());
    ft->prepare();
    check(waitFor([&]() { return pendingContentAccept.task() != nullptr; }),
          "file-transfer extension did not serialize content-accept");

    bool  ftBound = false, ftRequired = false;
    auto *provisional = icePad->groupedConnectionFor(ftTransport.data(), &ftBound, &ftRequired);
    check(ftBound && ftRequired && provisional == shared && icePad->liveAssociationCount() == 1,
          "provisional file-transfer extension did not reuse the established association");
    check(session.negotiatedGroupings().first().contents == QStringList({ offer.audioName, offer.videoName }),
          "provisional physical binding leaked into negotiated topology");

    if (scenario == QLatin1String("accept-error")) {
        RootTaskKeeper pendingCancellation(client.rootTask());
        acknowledgeJingleTask(pendingContentAccept, peer, false);
        check(ft->state() == J::State::Finished && session.state() == J::State::Active
                  && session.negotiatedGroupings().first().contents == QStringList({ offer.audioName, offer.videoName })
                  && !ftTransport->rtpAssociation() && icePad->liveAssociationCount() == 1,
              "failed content-accept IQ changed the existing BUNDLE or left provisional ownership live");
        check(waitFor([&]() { return pendingCancellation.task() != nullptr; }),
              "failed answer IQ left the peer's original offer unresolved");
        acknowledgeJingleTask(pendingCancellation, peer);
        return;
    }
    if (scenario == QLatin1String("destroy-and-reoffer")) {
        delete ft;
        check(!ftTransport->rtpAssociation(), "destroyed pending content retained runtime ownership");
        const QString nextName = QStringLiteral("next-active-file");
        QDomDocument  nextDoc;
        auto          nextAdd
            = activeFileAddPayload(nextDoc, session, offer, nextName, { offer.audioName, offer.videoName, nextName });
        check(session.updateFromXml(J::Action::ContentAdd, nextAdd),
              "new extension could not be offered after the previous content was destroyed");
        auto next = session.content(nextName, J::Origin::Initiator);
        check(next, "new extension disappeared");
        next->prepare();
        RootTaskKeeper pendingNextAccept(client.rootTask());
        acknowledgeJingleTask(pendingContentAccept, peer);
        check(session.state() == J::State::Active
                  && session.negotiatedGroupings().first().contents
                      == QStringList({ offer.audioName, offer.videoName }),
              "stale answer IQ completion committed the replacement extension or terminated the session");
        check(waitFor([&]() { return pendingNextAccept.task() != nullptr; }),
              "replacement extension did not send its own content-accept");
        acknowledgeJingleTask(pendingNextAccept, peer);
        check(session.negotiatedGroupings().first().contents
                      == QStringList({ offer.audioName, offer.videoName, nextName })
                  && icePad->liveAssociationCount() == 1,
              "replacement extension did not commit on its own answer IQ result");
        return;
    }
    if (scenario == QLatin1String("remove-member") || scenario == QLatin1String("destroy-member")) {
        RootTaskKeeper pendingCancellation(client.rootTask());
        if (scenario == QLatin1String("destroy-member")) {
            delete audio;
        } else {
            QDomDocument removeDoc;
            auto         remove = removeDoc.createElementNS(J::NS, QStringLiteral("jingle"));
            remove.appendChild(J::ContentBase(J::Origin::Initiator, offer.audioName)
                                   .toXml(&removeDoc, QStringLiteral("content"), J::NS));
            check(session.updateFromXml(J::Action::ContentRemove, remove),
                  "established member removal failed during pending extension");
        }
        acknowledgeJingleTask(pendingContentAccept, peer);
        check(session.state() == J::State::Active
                  && session.negotiatedGroupings().first().contents == QStringList { offer.videoName }
                  && !ftTransport->rtpAssociation() && !audioTransport->rtpAssociation(),
              "late answer IQ resurrected removed contents or committed an invalidated group snapshot");
        check(waitFor([&]() { return pendingCancellation.task() != nullptr; }),
              "invalidated extension did not cancel its proposed content");
        acknowledgeJingleTask(pendingCancellation, peer);
        check(ft->state() == J::State::Finished && videoTransport->rtpAssociation()
                  && icePad->liveAssociationCount() == 1,
              "cancelling an extension invalidated the surviving RTP member");
        if (scenario == QLatin1String("remove-member")) {
            const QString nextName = QStringLiteral("singleton-extension");
            QDomDocument  nextDoc;
            auto nextAdd = activeFileAddPayload(nextDoc, session, offer, nextName, { offer.videoName, nextName });
            check(session.updateFromXml(J::Action::ContentAdd, nextAdd),
                  "surviving BUNDLE member could not receive a new extension");
            auto           next = session.content(nextName, J::Origin::Initiator);
            RootTaskKeeper pendingNext(client.rootTask());
            next->prepare();
            check(waitFor([&]() { return pendingNext.task() != nullptr; }),
                  "singleton extension did not prepare its answer");
            acknowledgeJingleTask(pendingNext, peer);
            check(session.negotiatedGroupings().first().contents == QStringList({ offer.videoName, nextName })
                      && next->transport().staticCast<J::ICE::Transport>()->rtpAssociation()
                          == videoTransport->rtpAssociation(),
                  "extension of a surviving member did not reuse its association");
        } else {
            QPointer<J::ICE::IceConnection> oldConnection(shared);
            QDomDocument                    replaceDoc;
            auto replace = replacementPayload(replaceDoc, offer, { qMakePair(offer.videoName, offer.videoName) });
            RootTaskKeeper pendingReplaceAccept(client.rootTask());
            check(session.updateFromXml(J::Action::TransportReplace, replace),
                  "replacement of the surviving BUNDLE member failed");
            check(waitFor([&]() { return pendingReplaceAccept.task() != nullptr; }),
                  "replacement of singleton BUNDLE did not send transport-accept");
            acknowledgeJingleTask(pendingReplaceAccept, peer);
            check(!oldConnection && video->transport() != videoTransport
                      && video->transport().staticCast<J::ICE::Transport>()->rtpAssociation()
                      && icePad->liveAssociationCount() == 1,
                  "singleton replacement retained the old association or lost the new one");
        }
        return;
    }
    QDomDocument replaceDoc;
    auto         replace = replacementPayload(
        replaceDoc, offer,
        { qMakePair(offer.audioName, offer.audioName), qMakePair(offer.videoName, offer.videoName) });
    check(!session.updateFromXml(J::Action::TransportReplace, replace) && audio->transport() == audioTransport
              && video->transport() == videoTransport,
          "transport replacement raced a pending extension on the same BUNDLE");

    acknowledgeJingleTask(pendingContentAccept, peer);
    check(waitFor([&]() {
              const auto groups = session.negotiatedGroupings();
              return groups.size() == 1
                  && groups.first().contents == QStringList({ offer.audioName, offer.videoName, fileName });
          }),
          "content-accept acknowledgement did not commit active BUNDLE extension");
    check(icePad->liveAssociationCount() == 1, "committed active BUNDLE extension allocated another association");

    bool committedBound = false, committedRequired = false;
    check(icePad->groupedConnectionFor(ftTransport.data(), &committedBound, &committedRequired) == shared
              && committedBound && committedRequired,
          "committed file-transfer content did not remain on established association");

    session.setAutomaticGroupingEnabled(false);
    const QString  unbundledName = QStringLiteral("unbundled-file");
    QDomDocument   unbundledDoc;
    auto           unbundledAdd = activeFileAddPayload(unbundledDoc, session, offer, unbundledName,
                                                       { offer.audioName, offer.videoName, fileName, unbundledName });
    RootTaskKeeper pendingPolicyReject(client.rootTask());
    check(session.updateFromXml(J::Action::ContentAdd, unbundledAdd),
          "BUNDLE extension that local policy cannot share was not handled");
    check(waitFor([&]() { return pendingPolicyReject.task() != nullptr; }),
          "local grouping policy did not serialize content-reject");
    acknowledgeJingleTask(pendingPolicyReject, peer);
    check(!session.content(unbundledName, J::Origin::Initiator) && session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                  == QStringList({ offer.audioName, offer.videoName, fileName })
              && icePad->liveAssociationCount() == 1,
          "unacceptable BUNDLE membership was accepted independently or mutated established topology");
    session.setAutomaticGroupingEnabled(true);

    const QString rejectedName = QStringLiteral("rejected-file");
    QDomDocument  rejectedDoc;
    auto          rejectedAdd = activeFileAddPayload(rejectedDoc, session, offer, rejectedName,
                                                     { offer.audioName, offer.videoName, fileName, rejectedName });
    check(session.updateFromXml(J::Action::ContentAdd, rejectedAdd),
          "second active BUNDLE extension was rejected before rollback test");
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    auto rejectedFt = dynamic_cast<J::FileTransfer::Application *>(session.content(rejectedName, J::Origin::Initiator));
    check(rejectedFt, "rollback fixture did not create second file-transfer application");

    // Stage the second content on the existing association, then reject it
    // locally before the queued ContentAccept can be serialized. Session must
    // roll back only the provisional membership before sending ContentReject.
    rejectedFt->prepare();
    auto rejectedTransport = qSharedPointerDynamicCast<J::ICE::Transport>(rejectedFt->transport());
    check(bool(rejectedTransport), "rollback fixture did not create ICE transport");
    bool  rejectedBound = false, rejectedRequired = false;
    auto *rejectedProvisional
        = icePad->groupedConnectionFor(rejectedTransport.data(), &rejectedBound, &rejectedRequired);
    check(rejectedBound && rejectedRequired && rejectedProvisional == shared && icePad->liveAssociationCount() == 1,
          "rollback fixture did not stage on the established BUNDLE association");

    RootTaskKeeper pendingReject(client.rootTask());
    rejectedFt->remove(J::Reason::Decline, QStringLiteral("declined by local policy"));
    check(rejectedFt->evaluateOutgoingUpdate().action == J::Action::ContentReject,
          "local refusal of pending content-add was not content-reject");
    check(waitFor([&]() { return pendingReject.task() != nullptr; }),
          "local rejection did not serialize content removal");
    acknowledgeJingleTask(pendingReject, peer);
    check(waitFor([&]() {
              const auto groups = session.negotiatedGroupings();
              return groups.size() == 1
                  && groups.first().contents == QStringList({ offer.audioName, offer.videoName, fileName })
                  && icePad->liveAssociationCount() == 1 && rejectedFt->state() == J::State::Finished;
          }),
          "local rejection changed established BUNDLE topology or lifetime");

    bool rejectedStillBound = false, rejectedStillRequired = false;
    check(icePad->groupedConnectionFor(rejectedTransport.data(), &rejectedStillBound, &rejectedStillRequired) == nullptr
              && rejectedStillBound && !rejectedStillRequired,
          "local rejection retained provisional BUNDLE membership or lost content identity");
    check(shared && audioTransport->state() < J::State::Finishing && videoTransport->state() < J::State::Finishing,
          "extension rollback disturbed established RTP members");

    const QString malformedName = QStringLiteral("malformed-file");
    QDomDocument  malformedDoc;
    auto          malformed = activeFileAddPayload(malformedDoc, session, offer, malformedName,
                                                   { offer.videoName, offer.audioName, fileName, malformedName });
    check(!session.updateFromXml(J::Action::ContentAdd, malformed),
          "active BUNDLE extension accepted established-member reordering");
    check(!session.content(malformedName, J::Origin::Initiator) && icePad->liveAssociationCount() == 1,
          "malformed active BUNDLE extension mutated runtime state");

    // Session roles do not fix offer/answer roles for an active addition.
    // Here the original responder initiates the next content-add.
    std::unique_ptr<J::Application> localOwner(session.newContent(J::FileTransfer::NS, J::Origin::Responder));
    auto                            local = dynamic_cast<J::FileTransfer::Application *>(localOwner.get());
    check(local, "original responder could not create a local addition");
    J::FileTransfer::File localFile;
    localFile.setName(QStringLiteral("responder-added.bin"));
    localFile.setSize(32);
    localFile.addHash(Hash::from(Hash::Sha256, QByteArray("responder-active-offer")));
    local->setFile(localFile);
    RootTaskKeeper pendingLocalAdd(client.rootTask());
    session.addContent(localOwner.release());
    check(waitFor([&]() { return pendingLocalAdd.task() && local->state() == J::State::Unacked; }),
          "original responder's content-add did not reach the IQ boundary");
    acknowledgeJingleTask(pendingLocalAdd, peer);
    QDomDocument localAnswerDoc;
    auto         localAnswer  = localAnswerDoc.createElementNS(J::NS, QStringLiteral("jingle"));
    auto         localContent = J::ContentBase(J::Origin::Responder, local->contentName())
                            .toXml(&localAnswerDoc, QStringLiteral("content"), J::NS);
    localContent.appendChild(localAnswerDoc.importNode(static_cast<J::Application *>(local)->makeLocalOffer(), true));
    localContent.appendChild(localAnswerDoc.importNode(
        sourceContent(offer, offer.audioName).firstChildElement(QStringLiteral("transport")), true));
    localAnswer.appendChild(localContent);
    auto localGroup
        = localAnswerDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
    localGroup.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    auto localMembers = session.negotiatedGroupings().first().contents;
    localMembers.append(local->contentName());
    for (const auto &name : localMembers) {
        auto member = localAnswerDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                                     QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), name);
        localGroup.appendChild(member);
    }
    localAnswer.appendChild(localGroup);
    check(session.updateFromXml(J::Action::ContentAccept, localAnswer)
              && session.negotiatedGroupings().first().contents == localMembers
              && local->transport().staticCast<J::ICE::Transport>()->rtpAssociation()
                  == audioTransport->rtpAssociation(),
          "original responder's accepted addition did not extend the established BUNDLE");
}
#endif

#ifdef IRIS_TEST_SCTP
static void exerciseInitiatorActiveBundleExtension(const WireOffer &transportSource, TcpPortReserver *reserver,
                                                   const QString &scenario = {})
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("rfc9143-answer@example.test/device"));
    auto      features = rtpIcePeerFeatures(client, rtp);
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:s5b:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ibb:1"));
    features.removeAll(QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1"));
    setPeerFeatures(client, peer, features);

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto       audio
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto video
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(audio && video, "RFC 9143 answer fixture could not create RTP applications");

    RootTaskKeeper pendingInitiate(client.rootTask());
    session.initiate();
    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "RFC 9143 answer fixture did not select initial ICE transports");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    check(waitFor([&]() { return session.state() == J::State::Unacked; }),
          "RFC 9143 answer fixture did not serialize session-initiate");
    acknowledgeJingleTask(pendingInitiate, peer);
    check(session.state() == J::State::Pending, "RFC 9143 answer fixture did not reach pending session state");

    QDomDocument initialAcceptDoc;
    auto         initialAccept = sessionAcceptPayload(initialAcceptDoc, session, transportSource, audio, video, peer);
    check(session.updateFromXml(J::Action::SessionAccept, initialAccept) && session.state() == J::State::Active,
          "RFC 9143 answer fixture could not establish active RTP BUNDLE");
    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                  == QStringList({ audio->contentName(), video->contentName() })
              && icePad->liveAssociationCount() == 1,
          "RFC 9143 answer fixture did not establish one committed BUNDLE association");

    std::unique_ptr<J::Application> ftOwner(session.newContent(J::FileTransfer::NS, J::Origin::Initiator));
    auto                            ft = dynamic_cast<J::FileTransfer::Application *>(ftOwner.get());
    check(ft, "RFC 9143 answer fixture could not create file-transfer content");
    J::FileTransfer::File file;
    file.setName(QStringLiteral("rfc9143-answer.bin"));
    file.setSize(32);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("rfc9143-active-answer")));
    ft->setFile(file);

    RootTaskKeeper pendingAdd(client.rootTask());
    session.addContent(ftOwner.release());
    auto ftTransport = qSharedPointerDynamicCast<J::ICE::Transport>(ft->transport());
    check(bool(ftTransport), "active outgoing extension did not select ICE for file transfer");
    check(waitFor([&]() { return pendingAdd.task() != nullptr && ft->state() == J::State::Unacked; }),
          "active outgoing extension did not serialize content-add");

    bool  ftBound = false, ftRequired = false;
    auto *shared = icePad->groupedConnectionFor(ftTransport.data(), &ftBound, &ftRequired);
    check(shared && ftBound && ftRequired && icePad->liveAssociationCount() == 1,
          "outgoing extension did not stage file transfer on established BUNDLE association");
    if (scenario == QLatin1String("add-error")) {
        acknowledgeJingleTask(pendingAdd, peer, false);
        check(ft->state() == J::State::Finished && !ftTransport->rtpAssociation()
                  && session.negotiatedGroupings().first().contents
                      == QStringList({ audio->contentName(), video->contentName() })
                  && icePad->liveAssociationCount() == 1,
              "failed content-add IQ left provisional membership or damaged existing RTP");
        return;
    }
    acknowledgeJingleTask(pendingAdd, peer);
    check(ft->state() == J::State::Pending,
          "content-add acknowledgement did not leave the new content pending peer answer");

    if (scenario == QLatin1String("peer-reject")) {
        QDomDocument rejectDoc;
        auto         reject = rejectDoc.createElementNS(J::NS, QStringLiteral("jingle"));
        reject.appendChild(J::ContentBase(J::Origin::Initiator, ft->contentName())
                               .toXml(&rejectDoc, QStringLiteral("content"), J::NS));
        auto invalidMember = J::ContentBase(J::Origin::Initiator, audio->contentName())
                                 .toXml(&rejectDoc, QStringLiteral("content"), J::NS);
        reject.appendChild(invalidMember);
        check(!session.updateFromXml(J::Action::ContentReject, reject) && ft->state() == J::State::Pending,
              "malformed rejection batch partially rejected the proposed content");
        reject.removeChild(invalidMember);
        const auto name = ft->contentName();
        check(session.updateFromXml(J::Action::ContentReject, reject) && !session.content(name, J::Origin::Initiator)
                  && !ftTransport->rtpAssociation()
                  && session.negotiatedGroupings().first().contents
                      == QStringList({ audio->contentName(), video->contentName() })
                  && icePad->liveAssociationCount() == 1,
              "peer content-reject retained provisional ownership or changed established membership");
        return;
    }

    // RFC 9143 section 7.5.1 forbids accepting a newly added media/content
    // description while removing it from the BUNDLE in that same answer. The
    // peer must reject the content instead if it cannot accept the membership.
    QDomDocument invalidDoc;
    J::Jingle    invalidJingle(J::Action::ContentAccept, session.sid());
    auto         invalid = invalidJingle.toXml(&invalidDoc);
    invalidDoc.appendChild(invalid);
    J::ContentBase cb(J::Origin::Initiator, ft->contentName());
    cb.senders         = J::Origin::Initiator;
    auto answerContent = cb.toXml(&invalidDoc, QStringLiteral("content"), J::NS);
    answerContent.appendChild(invalidDoc.importNode(static_cast<J::Application *>(ft)->makeLocalOffer(), true));
    auto answerTransport = invalidDoc
                               .importNode(initialAccept.firstChildElement(QStringLiteral("content"))
                                               .firstChildElement(QStringLiteral("transport")),
                                           true)
                               .toElement();
    answerContent.appendChild(answerTransport);
    invalid.appendChild(answerContent);

    auto oldGroup
        = invalidDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
    oldGroup.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
    for (const auto &name : { audio->contentName(), video->contentName() }) {
        auto member
            = invalidDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("content"));
        member.setAttribute(QStringLiteral("name"), name);
        oldGroup.appendChild(member);
    }
    invalid.appendChild(oldGroup);

    check(!session.updateFromXml(J::Action::ContentAccept, invalid),
          "initiator accepted content-accept that removed the new member from BUNDLE");
    check(session.negotiatedGroupings().size() == 1
              && session.negotiatedGroupings().first().contents
                  == QStringList({ audio->contentName(), video->contentName() })
              && icePad->liveAssociationCount() == 1,
          "invalid unbundled content-accept mutated committed topology or association lifetime");

    bool stillBound = false, stillRequired = false;
    check(icePad->groupedConnectionFor(ftTransport.data(), &stillBound, &stillRequired) == shared && stillBound
              && stillRequired && ft->state() == J::State::Pending,
          "malformed answer cancelled the outstanding offer instead of refusing only the IQ");
    const auto revision = shared->generation.membershipRevision;

    auto member
        = invalidDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("content"));
    member.setAttribute(QStringLiteral("name"), ft->contentName());
    oldGroup.appendChild(member);
    // A complete group without the corresponding content definition is not an answer.
    invalid.removeChild(answerContent);
    check(!session.updateFromXml(J::Action::ContentAccept, invalid)
              && shared->generation.membershipRevision == revision,
          "empty content-accept committed a pending extension");
    invalid.insertBefore(answerContent, oldGroup);

    auto duplicate = answerContent.cloneNode(true);
    invalid.appendChild(duplicate);
    check(!session.updateFromXml(J::Action::ContentAccept, invalid), "duplicate content-accept was accepted");
    invalid.removeChild(duplicate);
    auto foreignContent = invalidDoc.createElementNS(QStringLiteral("urn:invalid:jingle"), QStringLiteral("content"));
    foreignContent.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
    foreignContent.setAttribute(QStringLiteral("name"), ft->contentName());
    for (auto child = answerContent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        foreignContent.appendChild(child.cloneNode(true));
    invalid.replaceChild(foreignContent, answerContent);
    check(!session.updateFromXml(J::Action::ContentAccept, invalid) && shared->generation.membershipRevision == revision
              && ft->state() == J::State::Pending,
          "foreign-namespace content definition mutated the pending BUNDLE transaction");
    invalid.replaceChild(answerContent, foreignContent);
    answerTransport.setAttribute(QStringLiteral("ufrag"), QStringLiteral("conflicting-credentials"));
    check(!session.updateFromXml(J::Action::ContentAccept, invalid),
          "extension answer changed the established ICE credentials");
    answerTransport.setAttribute(QStringLiteral("ufrag"), QStringLiteral("bundle-answer-ufrag"));
    check(shared->generation.membershipRevision == revision && ft->state() == J::State::Pending,
          "invalid answer mutated committed membership or application state");

    // A second, independent content-add can be answered while the BUNDLE
    // offer is pending. That answer may not commit the first content's group.
    std::unique_ptr<J::Application> independentOwner(session.newContent(J::FileTransfer::NS, J::Origin::Initiator));
    auto                            independent = dynamic_cast<J::FileTransfer::Application *>(independentOwner.get());
    check(independent, "independent pending content fixture failed");
    independent->setFile(file);
    RootTaskKeeper pendingIndependentAdd(client.rootTask());
    session.addContent(independentOwner.release());
    check(waitFor([&]() { return pendingIndependentAdd.task() && independent->state() == J::State::Unacked; }),
          "independent content-add did not reach the IQ boundary");
    acknowledgeJingleTask(pendingIndependentAdd, peer);
    QDomDocument independentDoc;
    auto         independentAnswer  = independentDoc.createElementNS(J::NS, QStringLiteral("jingle"));
    auto         independentContent = J::ContentBase(J::Origin::Initiator, independent->contentName())
                                  .toXml(&independentDoc, QStringLiteral("content"), J::NS);
    independentContent.appendChild(
        independentDoc.importNode(static_cast<J::Application *>(independent)->makeLocalOffer(), true));
    independentContent.appendChild(independentDoc.importNode(answerTransport, true));
    independentAnswer.appendChild(independentContent);
    auto unrelatedGroup = independentDoc.importNode(oldGroup, true);
    independentAnswer.appendChild(unrelatedGroup);
    check(!session.updateFromXml(J::Action::ContentAccept, independentAnswer) && ft->state() == J::State::Pending
              && independent->state() == J::State::Pending,
          "answer for another content committed or cancelled the pending BUNDLE offer");
    independentAnswer.removeChild(unrelatedGroup);
    check(session.updateFromXml(J::Action::ContentAccept, independentAnswer) && ft->state() == J::State::Pending
              && shared->generation.membershipRevision == revision,
          "unrelated independent acceptance disturbed the BUNDLE transaction");
    delete independent;

    check(session.updateFromXml(J::Action::ContentAccept, invalid),
          "corrected, fully bundled content-accept was rejected");
    check(session.negotiatedGroupings().first().contents
                  == QStringList({ audio->contentName(), video->contentName(), ft->contentName() })
              && shared->generation.membershipRevision == revision + 1 && icePad->liveAssociationCount() == 1,
          "successful local extension did not commit exactly one membership on the old association");

    // Application ownership determines membership even if callers retain a Transport.
    const auto   fileName = ft->contentName();
    QDomDocument removalDoc;
    auto         removal = removalDoc.createElementNS(J::NS, QStringLiteral("jingle"));
    removal.appendChild(
        J::ContentBase(J::Origin::Initiator, fileName).toXml(&removalDoc, QStringLiteral("content"), J::NS));
    check(session.updateFromXml(J::Action::ContentRemove, removal), "removal of a committed extension failed");
    check(!session.content(fileName, J::Origin::Initiator)
              && session.negotiatedGroupings().first().contents
                  == QStringList({ audio->contentName(), video->contentName() })
              && icePad->liveAssociationCount() == 1 && !ftTransport->rtpAssociation(),
          "removed extension retained topology or network ownership through a retained Transport");
}
#endif

static void exerciseInitiatorReplacement(const WireOffer &transportSource, TcpPortReserver *reserver)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("responder@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto       audio
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto video
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(audio && video, "replacement initiator could not create RTP applications");
    check(session.setGroupings(
              { J::ContentGroup { QStringLiteral("BUNDLE"), { audio->contentName(), video->contentName() } } }),
          "replacement initiator could not offer BUNDLE");

    // Exercise the real outgoing session-initiate state machine. initiate()
    // owns the production preparation boundary: RTP selects ICE synchronously
    // from prepare(), while media/DTLS completion and stanza serialization are
    // asynchronous. Do not inspect transport selection before this call.
    RootTaskKeeper pendingInitiate(client.rootTask());
    session.initiate();

    auto audioTransport = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto videoTransport = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(audioTransport && videoTransport, "replacement initiator did not select ICE during initiate()");
    auto icePad = audioTransport->pad().staticCast<J::ICE::Pad>();

    // There is no connected XMPP stream in this test process, so Task::go()
    // intentionally stops at the wire boundary; inject the peer's real IQ result
    // through the existing Task parser to complete exactly that transaction.
    check(waitFor([&]() { return session.state() == J::State::Unacked; }),
          "replacement initiator did not serialize session-initiate");
    acknowledgeJingleTask(pendingInitiate, peer);
    check(session.state() == J::State::Pending && audio->state() == J::State::Pending
              && video->state() == J::State::Pending,
          "session-initiate IQ result did not establish the pending negotiation boundary");
    check(icePad->liveAssociationCount() == 1,
          "outgoing negotiated BUNDLE offer did not retain one shared association");

    // Feed a genuine session-accept document through the same production parser
    // used by JTPush. The payload uses production RTP description serialization
    // and a production-generated ICE transport snapshot.
    QDomDocument acceptDoc;
    auto         accept = sessionAcceptPayload(acceptDoc, session, transportSource, audio, video, peer);
    check(session.updateFromXml(J::Action::SessionAccept, accept), "production session-accept XML was rejected");
    check(session.state() == J::State::Active && audio->state() == J::State::Accepted
              && video->state() == J::State::Accepted,
          "session-accept XML did not establish the active negotiated session");

    bool  audioBound = false, audioRequired = false, videoBound = false, videoRequired = false;
    auto *audioNetwork = icePad->groupedConnectionFor(audioTransport.data(), &audioBound, &audioRequired);
    auto *videoNetwork = icePad->groupedConnectionFor(videoTransport.data(), &videoBound, &videoRequired);
    check(audioBound && videoBound && audioRequired && videoRequired && audioNetwork && audioNetwork == videoNetwork
              && icePad->liveAssociationCount() == 1,
          "session-accept did not preserve the original shared BUNDLE association");
    QPointer<J::ICE::IceConnection> oldNetwork(audioNetwork);

    QDomDocument partialDoc;
    auto         partial = replacementPayload(partialDoc, transportSource,
                                              { qMakePair(audio->contentName(), transportSource.audioName) });
    check(!session.updateFromXml(J::Action::TransportReplace, partial),
          "partial negotiated BUNDLE transport-replace was accepted");
    check(audio->transport() == audioTransport && video->transport() == videoTransport && oldNetwork
              && icePad->liveAssociationCount() == 1,
          "partial BUNDLE transport-replace mutated the live association");

    QDomDocument fullDoc;
    auto         full = replacementPayload(fullDoc, transportSource,
                                           { qMakePair(audio->contentName(), transportSource.audioName),
                                             qMakePair(video->contentName(), transportSource.videoName) });
    check(session.updateFromXml(J::Action::TransportReplace, full),
          "full negotiated BUNDLE transport-replace was rejected");

    auto replacementAudio = qSharedPointerDynamicCast<J::ICE::Transport>(audio->transport());
    auto replacementVideo = qSharedPointerDynamicCast<J::ICE::Transport>(video->transport());
    check(replacementAudio && replacementVideo && replacementAudio != audioTransport
              && replacementVideo != videoTransport,
          "full BUNDLE transport-replace did not install fresh ICE transports");

    // Do not call groupedConnectionFor() here: Application::setTransport()
    // has queued the real RTP prepareTransport() path for both replacements.
    // Observe those production callbacks instead. The first prepared BUNDLE
    // member must leave the old association live; the second completes the
    // staged generation and atomically retires it.
    int  preparedReplacements = 0;
    bool audioPrepared        = false;
    bool videoPrepared        = false;
    auto observePrepared      = [&](J::ICE::Transport *transport, bool &seen) {
        if (seen || transport->state() != J::State::ApprovedToSend)
            return;
        seen = true;
        ++preparedReplacements;
        check(icePad->liveAssociationCount() == 1,
                   "replacement preparation exposed more than one live BUNDLE association");
        check(preparedReplacements <= 2, "replacement transport prepared more than once");
    };
    QObject::connect(replacementAudio.data(), &J::Transport::stateChanged, replacementAudio.data(),
                     [&]() { observePrepared(replacementAudio.data(), audioPrepared); });
    QObject::connect(replacementVideo.data(), &J::Transport::stateChanged, replacementVideo.data(),
                     [&]() { observePrepared(replacementVideo.data(), videoPrepared); });

    check(waitFor([&]() {
              return audioPrepared && videoPrepared && replacementAudio->rtpAssociation()
                  && replacementVideo->rtpAssociation();
          }),
          "production replacement preparation did not complete both BUNDLE members");
    check(!oldNetwork && icePad->liveAssociationCount() == 1,
          "full BUNDLE replacement did not leave exactly one live association");
    check(replacementAudio->rtpAssociation() == replacementVideo->rtpAssociation(),
          "replacement BUNDLE members did not share one SRTP session");
}

static void exerciseTwoGroupReplacement(const WireOffer &transportSource, TcpPortReserver *reserver)
{
    Client client;
    client.setTcpPortReserver(reserver);
    client.jingleICEManager()->setSelfAddress(QHostAddress::LocalHost);
    auto rtp = client.jingleManager()->rtpManager();
    rtp->setMediaProvider(std::make_shared<Provider>());
    rtp->setTransportNamespaces({ J::ICE::NS });

    const Jid peer(QStringLiteral("responder@example.test/device"));
    setPeerFeatures(client, peer, rtpIcePeerFeatures(client, rtp));

    J::Session session(client.jingleManager(), peer, J::Origin::Initiator);
    auto       a1
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto a2
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    auto b1
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("audio"), J::Origin::Both));
    auto b2
        = dynamic_cast<J::RTP::Application *>(rtp->createOutgoing(&session, QStringLiteral("video"), J::Origin::Both));
    check(a1 && a2 && b1 && b2, "two-group fixture could not create RTP applications");

    const QList<J::ContentGroup> groups { { QStringLiteral("BUNDLE"), { a1->contentName(), a2->contentName() } },
                                          { QStringLiteral("BUNDLE"), { b1->contentName(), b2->contentName() } } };
    check(session.setGroupings(groups), "two-group BUNDLE proposal rejected");

    RootTaskKeeper pendingInitiate(client.rootTask());
    session.initiate();

    auto ta1 = qSharedPointerDynamicCast<J::ICE::Transport>(a1->transport());
    auto ta2 = qSharedPointerDynamicCast<J::ICE::Transport>(a2->transport());
    auto tb1 = qSharedPointerDynamicCast<J::ICE::Transport>(b1->transport());
    auto tb2 = qSharedPointerDynamicCast<J::ICE::Transport>(b2->transport());
    check(ta1 && ta2 && tb1 && tb2, "two-group fixture did not select ICE");
    auto icePad = ta1->pad().staticCast<J::ICE::Pad>();

    check(waitFor([&]() { return session.state() == J::State::Unacked; }),
          "two-group fixture did not serialize session-initiate");
    acknowledgeJingleTask(pendingInitiate, peer);
    check(session.state() == J::State::Pending, "two-group initiate acknowledgement failed");
    check(icePad->liveAssociationCount() == 2, "two BUNDLE groups did not stage two associations");

    QDomDocument acceptDoc;
    J::Jingle    acceptJingle(J::Action::SessionAccept, session.sid());
    acceptJingle.setResponder(peer);
    auto accept = acceptJingle.toXml(&acceptDoc);
    acceptDoc.appendChild(accept);

    auto appendAnswer = [&](J::RTP::Application *application, const QString &sourceName, quint32 ssrc,
                            const QString &ufrag, const QString &pwd) {
        const auto local = application->localDescription();
        check(local.has_value(), "two-group local RTP offer disappeared");
        auto answer = *local;
        answer.ssrc = ssrc;

        const auto source = sourceContent(transportSource, sourceName);
        check(!source.isNull(), "two-group accept source content missing");
        auto sourceTransport = source.firstChildElement(QStringLiteral("transport"));
        check(!sourceTransport.isNull(), "two-group accept source transport missing");

        J::ContentBase cb(J::Origin::Initiator, application->contentName());
        cb.senders   = J::Origin::Both;
        auto content = cb.toXml(&acceptDoc, QStringLiteral("content"), J::NS);
        content.appendChild(answer.toXml(acceptDoc));

        auto transport = acceptDoc.importNode(sourceTransport, true).toElement();
        transport.setAttribute(QStringLiteral("ufrag"), ufrag);
        transport.setAttribute(QStringLiteral("pwd"), pwd);
        auto fingerprint = transport.firstChildElement(QStringLiteral("fingerprint"));
        if (!fingerprint.isNull())
            fingerprint.setAttribute(QStringLiteral("setup"), QStringLiteral("passive"));
        content.appendChild(transport);
        accept.appendChild(content);
    };

    appendAnswer(a1, transportSource.audioName, 0x51000001u, QStringLiteral("group-a-answer"),
                 QStringLiteral("group-a-password"));
    appendAnswer(a2, transportSource.videoName, 0x51000002u, QStringLiteral("group-a-answer"),
                 QStringLiteral("group-a-password"));
    appendAnswer(b1, transportSource.audioName, 0x52000001u, QStringLiteral("group-b-answer"),
                 QStringLiteral("group-b-password"));
    appendAnswer(b2, transportSource.videoName, 0x52000002u, QStringLiteral("group-b-answer"),
                 QStringLiteral("group-b-password"));

    for (const auto &groupSpec : groups) {
        auto group
            = acceptDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"), QStringLiteral("group"));
        group.setAttribute(QStringLiteral("semantics"), QStringLiteral("BUNDLE"));
        for (const auto &name : groupSpec.contents) {
            auto member = acceptDoc.createElementNS(QStringLiteral("urn:xmpp:jingle:apps:grouping:0"),
                                                    QStringLiteral("content"));
            member.setAttribute(QStringLiteral("name"), name);
            group.appendChild(member);
        }
        accept.appendChild(group);
    }

    check(session.updateFromXml(J::Action::SessionAccept, accept), "two-group session-accept was rejected");
    check(session.state() == J::State::Active, "two-group session did not become active");

    bool  a1Bound = false, a1Required = false, a2Bound = false, a2Required = false;
    bool  b1Bound = false, b1Required = false, b2Bound = false, b2Required = false;
    auto *networkA  = icePad->groupedConnectionFor(ta1.data(), &a1Bound, &a1Required);
    auto *networkA2 = icePad->groupedConnectionFor(ta2.data(), &a2Bound, &a2Required);
    auto *networkB  = icePad->groupedConnectionFor(tb1.data(), &b1Bound, &b1Required);
    auto *networkB2 = icePad->groupedConnectionFor(tb2.data(), &b2Bound, &b2Required);
    check(a1Bound && a2Bound && b1Bound && b2Bound && a1Required && a2Required && b1Required && b2Required && networkA
              && networkA == networkA2 && networkB && networkB == networkB2 && networkA != networkB
              && icePad->liveAssociationCount() == 2,
          "two-group negotiated topology was not two independent shared associations");

    QPointer<J::ICE::IceConnection> oldA(networkA);
    QPointer<J::ICE::IceConnection> stableB(networkB);

    QDomDocument replaceADoc;
    auto         replaceA = replacementPayload(replaceADoc, transportSource,
                                               { qMakePair(a1->contentName(), transportSource.audioName),
                                                 qMakePair(a2->contentName(), transportSource.videoName) });
    check(session.updateFromXml(J::Action::TransportReplace, replaceA), "first BUNDLE group replacement was rejected");

    auto ra1 = qSharedPointerDynamicCast<J::ICE::Transport>(a1->transport());
    auto ra2 = qSharedPointerDynamicCast<J::ICE::Transport>(a2->transport());
    check(ra1 && ra2 && ra1 != ta1 && ra2 != ta2, "first group did not install replacement transports");
    check(waitFor([&]() {
              return ra1->state() == J::State::ApprovedToSend && ra2->state() == J::State::ApprovedToSend
                  && ra1->rtpAssociation() && ra2->rtpAssociation();
          }),
          "first BUNDLE group replacement did not finish preparation");
    check(!oldA && stableB && icePad->liveAssociationCount() == 2,
          "replacing first BUNDLE group destroyed or duplicated sibling association");

    bool rb1 = false, rq1 = false, rb2 = false, rq2 = false;
    check(icePad->groupedConnectionFor(tb1.data(), &rb1, &rq1) == stableB
              && icePad->groupedConnectionFor(tb2.data(), &rb2, &rq2) == stableB && rb1 && rb2 && rq1 && rq2,
          "first group replacement changed second group identity");

    bool                            stableABound = false, stableARequired = false;
    QPointer<J::ICE::IceConnection> stableA(icePad->groupedConnectionFor(ra1.data(), &stableABound, &stableARequired));
    check(stableABound && stableARequired && stableA && stableA != stableB, "first replacement association missing");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    TcpPortReserver  reserver;

    Client     initiator;
    const auto offer          = makeOffer(initiator, &reserver, J::RTP::Media::Audio | J::RTP::Media::Video);
    const auto audioOnlyOffer = makeOffer(initiator, &reserver, J::RTP::Media::Audio);
    const auto videoOnlyOffer = makeOffer(initiator, &reserver, J::RTP::Media::Video);
    const auto udpOffer
        = makeOffer(initiator, &reserver, J::RTP::Media::Audio | J::RTP::Media::Video, J::ICE::NS_ICE_UDP);
    exerciseInitialSingletonAnswer(audioOnlyOffer, &reserver, true);
    exerciseInitialSingletonAnswer(audioOnlyOffer, &reserver, false);
    for (const auto &scenario :
         { QStringLiteral("success"), QStringLiteral("error"), QStringLiteral("remove"), QStringLiteral("destroy") }) {
        exerciseScreenAnswer(offer, &reserver, scenario);
        exerciseScreenAnswer(audioOnlyOffer, &reserver, scenario);
        exerciseScreenAnswer(videoOnlyOffer, &reserver, scenario);
        exerciseScreenAnswer(udpOffer, &reserver, scenario);
    }
    exerciseScreenAnswer(offer, &reserver, QStringLiteral("success"), false);
    exerciseScreenAnswer(udpOffer, &reserver, QStringLiteral("success"), false);
    exerciseResponder(offer, &reserver, true);
    exerciseResponder(offer, &reserver, false);
#ifdef IRIS_TEST_SCTP
    exerciseMixedRtpFileTransferBundle(&reserver);
    exerciseActiveFileTransferBundleExtension(offer, &reserver);
    exerciseInitiatorActiveBundleExtension(offer, &reserver);
    exerciseInitiatorActiveBundleExtension(offer, &reserver, QStringLiteral("add-error"));
    exerciseInitiatorActiveBundleExtension(offer, &reserver, QStringLiteral("peer-reject"));
    exerciseActiveFileTransferBundleExtension(offer, &reserver, QStringLiteral("accept-error"));
    exerciseActiveFileTransferBundleExtension(offer, &reserver, QStringLiteral("destroy-and-reoffer"));
    exerciseActiveFileTransferBundleExtension(offer, &reserver, QStringLiteral("remove-member"));
    exerciseActiveFileTransferBundleExtension(offer, &reserver, QStringLiteral("destroy-member"));
#endif
    exerciseInitiatorReplacement(offer, &reserver);
    exerciseTwoGroupReplacement(offer, &reserver);

    qInfo("BUNDLE signaling-to-runtime regressions passed");
    return 0;
}
