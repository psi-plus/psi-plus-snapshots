// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <QtCrypto>
#include <iris/jingle-application.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_task.h>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class Result : public Task {
public:
    Result(Task *parent, bool ok) : Task(parent)
    {
        if (ok)
            setSuccess();
        else
            setError(500);
    }
};

class TestPad : public J::ApplicationManagerPad {
public:
    explicit TestPad(J::Session *session) : session_(session) { }

    J::Session            *session() const override { return session_; }
    QString                ns() const override { return QStringLiteral("urn:iris:test:application"); }
    J::ApplicationManager *manager() const override { return nullptr; }
    QString                generateContentName(J::Origin) override { return QStringLiteral("test"); }

private:
    J::Session *session_;
};

class TestTransport : public J::Transport {
public:
    TestTransport() : Transport({}, J::Origin::Initiator) { setState(J::State::Active); }

    void                           prepare() override { }
    void                           start() override { }
    bool                           update(const QDomElement &) override { return true; }
    bool                           hasUpdates() const override { return false; }
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool) override { return {}; }
    bool                           isValid() const override { return true; }
    J::TransportFeatures           features() const override { return {}; }
    J::Connection::Ptr             addChannel(J::TransportFeatures, const QString &, int) override { return {}; }
    QList<J::Connection::Ptr>      channels() const override { return {}; }
};

class TestApplication : public J::Application {
public:
    explicit TestApplication(J::Session *session, J::Origin senders = J::Origin::Both)
    {
        _pad.reset(new TestPad(session));
        _contentName = QStringLiteral("audio");
        _creator     = J::Origin::Initiator;
        _senders     = senders;
    }

    void activate()
    {
        _transport.reset(new TestTransport);
        _state = J::State::Active;
    }

    void                                setState(J::State state) override { _state = state; }
    const std::optional<Stanza::Error> &lastError() const override { return error_; }
    J::Reason                           lastReason() const override { return {}; }
    SetDescError                        setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError                        setRemoteAnswer(const QDomElement &) override { return Unparsed; }
    QDomElement                         makeLocalOffer() override { return {}; }
    QDomElement                         makeLocalAnswer() override { return {}; }
    bool                                supportsContentModify() const override { return supportsModify_; }
    void                                prepare() override { }
    void                                start() override { }
    void                                remove(J::Reason::Condition, const QString &) override { }
    void                                incomingRemove(const J::Reason &) override { }

    bool supportsModify_ = true;

protected:
    void prepareTransport() override { }

private:
    std::optional<Stanza::Error> error_;
};

static QDomElement firstContent(const J::OutgoingUpdate &update)
{
    const auto &elements = std::get<0>(update);
    check(elements.size() == 1, "content-modify did not serialize exactly one content");
    return elements.first();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Client           client;
    J::Session       session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")));

    {
        TestApplication initial(&session);
        int             directionNotifications     = 0;
        int             peerDirectionNotifications = 0;
        QObject::connect(&initial, &J::Application::sendersChanged, &app, [&](J::Origin) { ++directionNotifications; });
        QObject::connect(&initial, &J::Application::sendersChangedByPeer, &app,
                         [&](J::Origin) { ++peerDirectionNotifications; });
        check(initial.requestSenders(J::Origin::Responder), "initial direction request rejected");
        check(initial.senders() == J::Origin::Responder, "initial direction was not changed synchronously");
        check(directionNotifications == 1, "initial direction change was not notified");
        check(peerDirectionNotifications == 0, "local initial direction was reported as peer-originated");
        check(initial.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "initial direction unexpectedly queued content-modify");
    }

    Result success(client.rootTask(), true), failure(client.rootTask(), false);

    {
        TestApplication active(&session, J::Origin::Responder);
        active.activate();
        int directionNotifications     = 0;
        int peerDirectionNotifications = 0;
        QObject::connect(&active, &J::Application::sendersChanged, &app, [&](J::Origin) { ++directionNotifications; });
        QObject::connect(&active, &J::Application::sendersChangedByPeer, &app,
                         [&](J::Origin) { ++peerDirectionNotifications; });

        check(active.requestSenders(J::Origin::Both), "active direction request rejected");
        check(active.senders() == J::Origin::Responder, "direction changed before content-modify ACK");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "active direction did not queue content-modify");
        auto update  = active.takeOutgoingUpdate();
        auto content = firstContent(update);
        check(content.attribute(QStringLiteral("senders")) == QLatin1String("both"),
              "content-modify omitted explicit senders=both");
        check(active.senders() == J::Origin::Responder, "serialized direction changed before ACK");
        std::get<1>(update)(&success);
        check(active.senders() == J::Origin::Both, "successful content-modify did not commit direction");
        check(directionNotifications == 1, "successful content-modify did not notify direction change once");
        check(peerDirectionNotifications == 0, "outgoing content-modify ACK was reported as peer-originated");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "successful direction request remained queued");
    }

    {
        auto disposable = new J::Session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/destructive")));
        auto active     = new TestApplication(disposable, J::Origin::Responder);
        disposable->addContent(active);
        active->activate();
        QPointer<J::Session> sessionGuard(disposable);
        QObject::connect(active, &J::Application::sendersChanged, active,
                         [disposable](J::Origin) { delete disposable; });

        check(active->requestSenders(J::Origin::Both), "destructive ACK direction request rejected");
        check(active->evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "destructive ACK direction was not queued");
        auto update = active->takeOutgoingUpdate();
        std::get<1>(update)(&success);
        check(!sessionGuard, "outgoing sendersChanged handler did not destroy the session");
    }

    {
        TestApplication active(&session);
        active.activate();
        int directionNotifications     = 0;
        int peerDirectionNotifications = 0;
        QObject::connect(&active, &J::Application::sendersChanged, &app, [&](J::Origin) { ++directionNotifications; });
        QObject::connect(&active, &J::Application::sendersChangedByPeer, &app,
                         [&](J::Origin) { ++peerDirectionNotifications; });

        active.incomingContentModify(J::Origin::Responder);
        check(active.senders() == J::Origin::Responder, "incoming content-modify did not change direction");
        check(directionNotifications == 1, "incoming content-modify did not notify general direction change");
        check(peerDirectionNotifications == 1, "incoming content-modify did not notify peer-originated change");

        active.incomingContentModify(J::Origin::Responder);
        check(directionNotifications == 1, "idempotent incoming content-modify notified general change twice");
        check(peerDirectionNotifications == 1, "idempotent incoming content-modify notified peer change twice");
    }

    {
        TestApplication active(&session);
        active.activate();
        int peerDirectionNotifications = 0;
        QObject::connect(&active, &J::Application::sendersChangedByPeer, &app,
                         [&](J::Origin) { ++peerDirectionNotifications; });

        check(active.requestSenders(J::Origin::Responder), "first superseding direction request rejected");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "first superseding direction was not queued");
        auto first = active.takeOutgoingUpdate();
        check(firstContent(first).attribute(QStringLiteral("senders")) == QLatin1String("responder"),
              "first superseding direction serialized incorrectly");

        check(active.requestSenders(J::Origin::Both), "newer direction request rejected while IQ was in flight");
        std::get<1>(first)(&success);
        check(active.senders() == J::Origin::Responder, "first ACK did not commit its negotiated direction");
        check(peerDirectionNotifications == 0, "superseded outgoing ACK was reported as peer-originated");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "newer direction intent was lost after first ACK");
        auto second = active.takeOutgoingUpdate();
        check(firstContent(second).attribute(QStringLiteral("senders")) == QLatin1String("both"),
              "newer direction did not serialize explicit senders=both");
        std::get<1>(second)(&success);
        check(active.senders() == J::Origin::Both, "newer direction was not committed after ACK");
        check(peerDirectionNotifications == 0, "newer outgoing ACK was reported as peer-originated");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction, "superseded direction remained queued");
    }

    {
        TestApplication active(&session, J::Origin::Responder);
        active.activate();
        bool reentered = false;
        QObject::connect(&active, &J::Application::sendersChanged, &app, [&](J::Origin senders) {
            if (!reentered && senders == J::Origin::Both) {
                reentered = true;
                check(active.requestSenders(J::Origin::Initiator),
                      "reentrant direction request from ACK notification was rejected");
            }
        });

        check(active.requestSenders(J::Origin::Both), "reentrant ACK setup request rejected");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "reentrant ACK setup direction was not queued");
        auto first = active.takeOutgoingUpdate();
        std::get<1>(first)(&success);
        check(reentered, "ACK did not invoke reentrant direction handler");
        check(active.senders() == J::Origin::Both, "ACK did not commit direction before reentrant request");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "reentrant direction intent was lost after ACK callback");
        auto second = active.takeOutgoingUpdate();
        check(firstContent(second).attribute(QStringLiteral("senders")) == QLatin1String("initiator"),
              "reentrant direction intent serialized incorrectly");
        std::get<1>(second)(&success);
        check(active.senders() == J::Origin::Initiator, "reentrant direction was not committed after ACK");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "reentrant direction remained queued after ACK");
    }

    {
        TestApplication active(&session);
        active.activate();
        check(active.requestSenders(J::Origin::Initiator), "failing direction request rejected locally");
        check(active.evaluateOutgoingUpdate().action == J::Action::ContentModify,
              "failing direction request was not queued");
        auto update = active.takeOutgoingUpdate();
        std::get<1>(update)(&failure);
        check(active.senders() == J::Origin::Both, "failed content-modify changed negotiated direction");
        check(active.evaluateOutgoingUpdate().action == J::Action::NoAction,
              "failed content-modify was retried forever");
    }

    {
        TestApplication fixed(&session);
        fixed.supportsModify_ = false;
        check(!fixed.requestSenders(J::Origin::Responder), "fixed-direction application accepted content-modify");
        check(fixed.senders() == J::Origin::Both, "rejected direction request changed application state");
    }

    qInfo("Outgoing content-modify direction regressions passed");
}
