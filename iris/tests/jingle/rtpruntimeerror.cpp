// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <iris/jingle-rtp.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <qca.h>

using namespace XMPP;
using namespace XMPP::Jingle;
namespace R = XMPP::Jingle::RTP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Endpoint final : public R::MediaEndpoint {
public:
    explicit Endpoint(QString media) : media_(std::move(media)) { }
    R::Description localOffer() const override
    {
        R::Description result;
        result.media   = media_;
        result.rtcpMux = true;
        R::PayloadType payload;
        payload.id        = 111;
        payload.name      = QStringLiteral("opus");
        payload.clockrate = 48000;
        payload.channels  = 2;
        result.payloads.append(payload);
        return result;
    }
    std::optional<R::Description> makeAnswer(const R::Description &offer) const override { return offer; }
    bool acceptsAnswer(const R::Description &, const R::Description &) const override { return true; }
    bool configure(const R::Description &, const R::Description &) override { return true; }
    void stop() override { }

private:
    QString media_;
};

class MediaSession final : public R::MediaSession {
public:
    std::unique_ptr<R::MediaEndpoint> createEndpoint(const QString &, const QString &media) override
    {
        return std::make_unique<Endpoint>(media);
    }
    void fail() { emit runtimeError({ R::MediaError::Code::Backend, QStringLiteral("runtime backend failure") }); }
};

class Provider final : public R::MediaProvider {
public:
    std::unique_ptr<R::MediaSession> createSession() override
    {
        auto result = std::make_unique<MediaSession>();
        session     = result.get();
        return result;
    }
    MediaSession *session = nullptr;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    auto             manager  = client.jingleManager()->rtpManager();
    auto             provider = std::make_shared<Provider>();
    Session          session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));
    auto             pad = QSharedPointer<R::Pad>::create(manager, &session, provider, QStringList {});

    auto audio = std::make_unique<R::Application>(pad, QStringLiteral("audio"), Origin::Initiator, Origin::Both);
    auto video = std::make_unique<R::Application>(pad, QStringLiteral("video"), Origin::Initiator, Origin::Both);
    check(audio->initializeOutgoing(QStringLiteral("audio")) && video->initializeOutgoing(QStringLiteral("video"))
              && provider->session,
          "runtime-error fixture setup failed");

    provider->session->fail();
    check(audio->state() >= State::Finishing && video->state() >= State::Finishing,
          "runtime media failure did not terminate every live RTP content");
    check(audio->lastReason().condition() == Reason::FailedApplication
              && video->lastReason().condition() == Reason::FailedApplication,
          "runtime media failure used the wrong Jingle reason");

    qInfo("RTP runtime media failure regression passed");
}
