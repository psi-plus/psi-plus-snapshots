// SPDX-License-Identifier: LGPL-2.1-or-later
#define main iris_contentmodifyrace_legacy_main
#include "contentmodifyrace.cpp"
#undef main
#include <iris/xmpp-im/xmpp_externalservicediscovery.h>
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
    void write(const Stanza &stanza) override { written.append(stanza.element().cloneNode(true).toElement()); }
    QList<QDomElement> written;
};

static void flush()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

int main(int argc, char **argv)
{
    QCoreApplication                   app(argc, argv);
    QCA::Initializer                   qca;
    QPointer<ExternalServiceDiscovery> discovery;
    {
        Client owner;
        discovery = owner.externalServiceDiscovery();
        check(discovery, "Client has no external service discovery");
    }
    check(!discovery, "external service discovery outlived its Client");
    for (bool fromRetry : { false, true }) {
        NoNetworkConnector connector;
        RecordingStream    stream(&connector);
        Client             client;
        const Jid          peer("peer@example.test/device");
        client.connectToServer(&stream, Jid("local@example.test/device"));
        auto session = std::make_unique<J::Session>(client.jingleManager(), peer, J::Origin::Initiator);
        client.jingleManager()->registerSession(session.get());
        class PreparedApplication : public TestApplication {
        public:
            using TestApplication::TestApplication;
            void prepare() override { setState(J::State::ApprovedToSend); }
        };
        auto content = new PreparedApplication(session.get());
        session->addContent(content);
        content->attachTransport();
        session->initiate();
        flush();
        check(!stream.written.isEmpty(), "fixture did not send session-initiate");
        const auto initial = stream.written.last();
        check(initial.firstChildElement("jingle").attribute("action") == "session-initiate",
              "fixture did not negotiate initial session");
        auto initialAck = client.doc()->createElement("iq");
        initialAck.setAttribute("from", peer.full());
        initialAck.setAttribute("id", initial.attribute("id"));
        initialAck.setAttribute("type", "result");
        check(client.rootTask()->take(initialAck), "fixture initial IQ acknowledgement was not consumed");
        content->activate();

        class Deleter : public J::TieBreaker::Resolver {
        public:
            std::function<void()>   destroy;
            J::TieBreaker::Solution resolve(const QDomElement &, const QDomElement &) override
            {
                return J::TieBreaker::Solution::Postpone;
            }
            void retry(const J::TieBreaker::RetryContext &) override { destroy(); }
        } deleter;
        deleter.destroy = [&] { session.reset(); };
        J::TieBreaker::Registration registration;
        if (fromRetry)
            registration = session->tieBreaker()->registerResolver(J::Action::ContentModify, &deleter);
        else
            QObject::connect(content, &J::Application::sendersAttemptFinished, &app,
                             [&](const J::Application::SendersAttemptResult &) { session.reset(); });

        content->requestSenders(J::Origin::Responder);
        flush();
        QDomElement sent;
        for (const auto &iq : stream.written) {
            if (iq.firstChildElement("jingle").attribute("action") == "content-modify")
                sent = iq;
        }
        check(!sent.isNull(), "real Session scheduler did not send content-modify");
        if (fromRetry) {
            const auto resolution = session->tieBreaker()->resolveIncoming(J::Action::ContentModify,
                                                                           client.doc()->createElement("jingle"));
            check(resolution.id != 0, "custom lifecycle resolver did not arm recovery");
            session->tieBreaker()->incomingFinished(resolution.id, J::TieBreaker::RemoteResult::Applied);
        }
        auto response = client.doc()->createElement("iq");
        response.setAttribute("from", peer.full());
        response.setAttribute("id", sent.attribute("id"));
        response.setAttribute("type", fromRetry ? "error" : "result");
        if (fromRetry) {
            Stanza::Error error(Stanza::Error::ErrorType::Cancel, Stanza::Error::ErrorCond::Conflict);
            response.appendChild(error.toXml(*client.doc(), "jabber:client"));
        }
        check(client.rootTask()->take(response), "real JT did not consume completion IQ");
        flush();
        check(!session, "completion callback did not destroy Session");
    }
    return 0;
}
