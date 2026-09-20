// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_transportreplace_correctness_main
#include "transportreplacecorrectness.cpp"
#undef main
#include <iris/xmpp.h>
#include <iris/xmpp_clientstream.h>

class NoNetworkConnector : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &stanza) override
    {
        written.append(stanza.element().cloneNode(true).toElement());
        const auto callback = onWrite;
        if (callback)
            callback();
    }
    QList<QDomElement>    written;
    std::function<void()> onWrite;
};

class AckTransport : public TestTransport {
public:
    using TestTransport::TestTransport;
    std::function<void()>          onAck;
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensure) override
    {
        auto [element, callback] = TestTransport::takeOutgoingUpdate(ensure);
        return { element, [guard = QPointer<AckTransport>(this), callback](Task *task) {
                    if (callback)
                        callback(task);
                    if (guard) {
                        const auto notify = guard->onAck;
                        if (notify)
                            notify();
                    }
                } };
    }
};

class PreparedApplication : public TestApplication {
public:
    using TestApplication::TestApplication;
    void prepare() override { setState(J::State::ApprovedToSend); }
};

class HintSelector : public TestSelector {
public:
    QSharedPointer<J::Transport> next;
    std::function<void()>        onHint;
    std::function<void()>        onReplace;
    bool                         hasMoreTransports() const override { return bool(next); }
    QSharedPointer<J::Transport> getAlikeTransport(QSharedPointer<J::Transport>) override
    {
        auto result   = std::exchange(next, {});
        auto callback = onHint;
        if (callback)
            callback();
        return result;
    }
    bool replace(QSharedPointer<J::Transport> oldTransport, QSharedPointer<J::Transport> newTransport) override
    {
        const bool result   = TestSelector::replace(std::move(oldTransport), std::move(newTransport));
        auto       callback = std::move(onReplace);
        if (callback)
            callback();
        return result;
    }
};

static void flush()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

struct Fixture {
    TestTransportManager                manager;
    NoNetworkConnector                  connector;
    RecordingStream                     stream { &connector };
    Client                              client;
    const Jid                           peer { "peer@example.test/device" };
    std::unique_ptr<J::Session>         owner { std::make_unique<J::Session>(client.jingleManager(), peer,
                                                                             J::Origin::Initiator) };
    J::Session                         &session { *owner };
    QList<PreparedApplication *>        apps;
    QList<QSharedPointer<AckTransport>> transports;

    Fixture()
    {
        client.connectToServer(&stream, Jid("local@example.test/device"));
        client.jingleManager()->registerTransport(&manager);
        client.jingleManager()->registerSession(&session);
        for (const auto &name : { QStringLiteral("audio"), QStringLiteral("video") }) {
            auto transport = QSharedPointer<AckTransport>::create(
                session.transportPadFactory(TestTransportManager::namespaceUri()), J::Origin::Initiator, name);
            transport->forceState(J::State::ApprovedToSend);
            transport->setHasUpdates(true);
            auto app = new PreparedApplication(&session, name, J::Origin::Initiator);
            app->markInitialApplication(true);
            app->installTransport(transport, std::make_unique<TestSelector>());
            session.addContent(app);
            transports.append(transport);
            apps.append(app);
        }
        session.initiate();
        flush();
        acknowledge(sent("session-initiate"), true);
        for (auto app : apps)
            app->setState(J::State::Connecting);
    }

    QDomElement sent(const QString &action) const
    {
        for (auto it = stream.written.crbegin(); it != stream.written.crend(); ++it) {
            if (it->firstChildElement("jingle").attribute("action") == action)
                return *it;
        }
        qFatal("missing outgoing %s", qPrintable(action));
        return {};
    }

    void acknowledge(const QDomElement &sent, bool success)
    {
        auto iq = client.doc()->createElement("iq");
        iq.setAttribute("from", peer.full());
        iq.setAttribute("id", sent.attribute("id"));
        iq.setAttribute("type", success ? "result" : "error");
        if (!success)
            iq.appendChild(J::ErrorUtil::makeTieBreak(*client.doc()).toXml(*client.doc(), "jabber:client"));
        check(client.rootTask()->take(iq), "real JT did not consume completion");
    }

    QDomElement propose(int count = 2)
    {
        for (int i = 0; i < count; ++i) {
            apps[i]->markReplacePlanned();
            transports[i]->setHasUpdates(true);
            emit apps[i]->updated();
        }
        flush();
        auto iq = sent("transport-replace");
        check(iq.firstChildElement("jingle").elementsByTagName("content").size() == count,
              "fixture did not batch both contents in one outgoing IQ");
        return iq;
    }

    QDomElement incoming(const QString &name)
    {
        QDomDocument doc;
        auto         jingle = makeReplace(doc, { "remote" });
        jingle.firstChildElement("content").setAttribute("name", name);
        jingle.setAttribute("sid", session.sid());
        jingle.setAttribute("action", "transport-replace");
        auto iq = client.doc()->createElement("iq");
        iq.setAttribute("from", peer.full());
        iq.setAttribute("id", "nested-replace");
        iq.setAttribute("type", "set");
        iq.appendChild(client.doc()->importNode(jingle, true));
        return iq;
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    for (bool success : { true, false }) {
        Fixture    f;
        const auto proposal  = f.propose();
        bool       delivered = false;
        // Whichever owner is completed first reenters for its not-yet-completed sibling.
        for (int i = 0; i < 2; ++i) {
            f.transports[i]->onAck = [&, i] {
                if (delivered)
                    return;
                delivered    = true;
                auto sibling = f.apps[1 - i];
                check(sibling->transportReplaceAwaitingAck(), "fixture already completed sibling callback");
                check(f.client.rootTask()->take(f.incoming(sibling->contentName())),
                      "JTPush did not consume replacement");
                check(f.stream.written.last().attribute("type") == "result",
                      "completed multi-content IQ caused a stale sibling tie-break");
                check(sibling->transport() != f.transports[1 - i], "sibling replacement was not installed");
            };
        }
        f.acknowledge(proposal, success);
        check(delivered, "transport completion callback did not run");
    }
    for (int scenario = 0; scenario < 11; ++scenario) {
        Fixture f;
        auto    videoOld    = makeTransport(f.session, J::Origin::Responder, J::State::Active, "video-old");
        auto    next        = makeTransport(f.session, J::Origin::Initiator, J::State::Created, "hint-successor");
        auto    selector    = std::make_unique<HintSelector>();
        selector->next      = next;
        selector->onReplace = [&] {
            if (scenario == 7)
                delete f.apps[1];
            if (scenario == 8)
                check(f.apps[1]->setTransport(videoOld), "selector replace could not reselect same transport");
        };
        int hints        = 0;
        selector->onHint = [&] {
            ++hints;
            check(f.stream.written.last().attribute("id") == "nested-replace"
                      && f.stream.written.last().attribute("type") == "error",
                  "sibling hint was consumed before the tie-break reply");
            if (scenario == 5)
                delete f.apps[1];
            if (scenario == 6)
                check(f.apps[1]->setTransport(videoOld), "hint callback could not reselect same transport");
            if (scenario == 10)
                f.owner.reset();
        };
        f.apps[1]->installTransport(videoOld, std::move(selector));
        f.propose(1); // only audio is in flight; video is an advisory sibling
        auto incoming = f.incoming("audio");
        auto payload  = incoming.firstChildElement("jingle");
        auto video    = payload.firstChildElement("content").cloneNode(true).toElement();
        video.setAttribute("name", "video");
        payload.appendChild(video);
        if (scenario == 1)
            video.firstChildElement("transport").setAttribute("parse", "fail");
        if (scenario == 4)
            video.setAttribute("name", "audio"); // malformed duplicate, not a collision outcome
        QPointer<PreparedApplication> videoGuard(f.apps[1]);
        f.stream.onWrite = [&] {
            if (f.stream.written.last().attribute("id") != "nested-replace")
                return;
            if (scenario == 2)
                check(f.apps[1]->setTransport(videoOld), "reply callback could not reselect same transport");
            if (scenario == 3)
                delete f.apps[1];
            if (scenario == 9)
                f.owner.reset();
        };
        check(f.client.rootTask()->take(incoming), "JTPush did not consume crossed replacement batch");
        check(f.stream.written.last().attribute("type") == "error", "initiator accepted crossed batch");
        const auto error     = f.stream.written.last().firstChildElement("error");
        const bool malformed = scenario == 1 || scenario == 4;
        check(!error.firstChildElement(malformed ? "bad-request" : "conflict").isNull(),
              "malformed batch and tie-break errors were conflated");
        check(hints == (scenario == 0 || (scenario >= 5 && scenario != 9) ? 1 : 0),
              "stale/malformed sibling reached hint selection");
        if (scenario >= 9) {
            check(!f.owner && !videoGuard, "callback failed to delete Session and its contents");
            continue;
        }
        check(f.apps[0]->transport() == f.transports[0], "losing batch replaced the local winning transport");
        if (scenario == 0)
            check(f.apps[1]->transport() == next, "validated hint did not select local successor");
        if (scenario == 2 || scenario == 6 || scenario == 8)
            check(f.apps[1]->transport() == videoOld, "post-reply hint overwrote same-object new generation");
        if (scenario == 3 || scenario == 5 || scenario == 7)
            check(!videoGuard, "reply callback did not delete sibling");
    }
    for (int remoteCase : { 0, 1, 2 })
        for (bool success : { false, true }) {
            TestTransportManager manager;
            NoNetworkConnector   connector;
            RecordingStream      stream(&connector);
            Client               client;
            client.connectToServer(&stream, Jid("local@example.test/device"));
            client.jingleManager()->registerTransport(&manager);
            const Jid  peer("initiator@example.test/device");
            J::Session session(client.jingleManager(), peer, J::Origin::Responder);
            const auto sid   = client.jingleManager()->registerSession(&session);
            auto       local = makeTransport(session, J::Origin::Responder, J::State::ApprovedToSend, "local");
            local->setHasUpdates(true);
            auto app = addApplication(session, local, std::make_unique<TestSelector>());
            app->markReplacePlanned();
            app->evaluateOutgoingUpdate();
            const auto update = app->takeOutgoingUpdate();
            auto       xml    = client.doc()->createElementNS(J::NS, "jingle");
            for (const auto &element : std::get<0>(update))
                xml.appendChild(element);
            const auto tx       = session.tieBreaker()->outgoingStarted(J::Action::TransportReplace, xml);
            auto       incoming = xml.cloneNode(true).toElement();
            incoming.setAttribute("sid", sid);
            incoming.setAttribute("action", "transport-replace");
            auto content = incoming.firstChildElement("content");
            if (remoteCase == 1)
                content.firstChildElement("transport").setAttribute("parse", "fail");
            if (remoteCase == 2)
                content.replaceChild(client.doc()->createElementNS("urn:iris:test:unsupported", "transport"),
                                     content.firstChildElement("transport"));
            auto iq = client.doc()->createElement("iq");
            iq.setAttribute("from", peer.full());
            iq.setAttribute("id", "initiator-winner");
            iq.setAttribute("type", "set");
            iq.appendChild(incoming);
            check(client.rootTask()->take(iq), "responder did not consume initiator replacement");
            check(stream.written.last().attribute("type") == (remoteCase == 1 ? "error" : "result"),
                  "responder did not use ordinary validation for the initiator action");
            const auto remote = app->transport();
            if (remoteCase == 0)
                check(remote != local && remote->creator() == J::Origin::Initiator,
                      "responder failed to install the winning remote transport");
            else
                check(remote == local, "rejected/unsupported remote action changed the local proposal");
            Result completion(client.rootTask(), success);
            session.tieBreaker()->outgoingFinished(
                tx, success ? std::optional<Stanza::Error>() : J::ErrorUtil::makeTieBreak(*client.doc()));
            std::get<1>(update)(&completion);
            session.tieBreaker()->outgoingCallbacksFinished(tx);
            std::get<1>(update)(&completion); // duplicate delivery cannot repeat fallback
            check(app->transport() == remote, "local completion resurrected a superseded transport");
            if (remoteCase == 0 || success) {
                check(app->transportReplaceInProgress() && app->state() == J::State::Connecting,
                      "accepted replacement was lost to duplicate fallback");
            } else {
                check(!app->transportReplaceAwaitingAck() && app->state() == J::State::Finishing,
                      "failed local proposal without a remote winner did not exhaust its selector");
            }
        }
    {
        Fixture    f;
        const auto proposal = f.propose(1);
        check(f.client.rootTask()->take(f.incoming("video")), "disjoint action was not consumed");
        check(f.stream.written.last().attribute("type") == "result", "disjoint content caused a tie-break");
        check(f.apps[0]->transport() == f.transports[0] && f.apps[1]->transport() != f.transports[1],
              "disjoint action modified the wrong content");
        f.acknowledge(proposal, true);
    }
    return 0;
}
