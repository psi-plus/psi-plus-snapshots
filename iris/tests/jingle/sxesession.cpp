// SPDX-License-Identifier: LGPL-2.1-or-later
// End-to-end Jingle SXE whiteboard session between two clients with looped-back streams.
#include <iris/jingle-session.h>
#include <iris/xmpp-im/jingle-sxe.h>
#include <iris/xmpp-im/xmpp_jinglemessage.h>
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>

#include <QCoreApplication>
#include <QPointer>
#include <QTextStream>
#include <QtCrypto>

using namespace XMPP;
namespace J   = XMPP::Jingle;
namespace SXE = XMPP::Jingle::SXE;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class NoNetworkConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream final : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &stanza) override { written.append(stanza.element().cloneNode(true).toElement()); }

    QList<QDomElement> written;
};

struct Endpoint {
    NoNetworkConnector connector;
    RecordingStream    stream { &connector };
    Client             client;
    Jid                jid;

    explicit Endpoint(const QString &user)
    {
        jid = Jid(user + QStringLiteral("@example.test/device"));
        client.start(QStringLiteral("example.test"), user, QString(), QStringLiteral("device"));
        client.connectToServer(&stream, jid);
    }
};

static QList<QDomElement> sentMessages;

// Delivers everything written by one endpoint to the other until both are quiet.
static void pump(Endpoint &a, Endpoint &b)
{
    for (int round = 0; round < 200; ++round) {
        bool moved = false;
        for (auto [from, to] : { std::pair<Endpoint *, Endpoint *> { &a, &b }, { &b, &a } }) {
            auto out = std::move(from->stream.written);
            from->stream.written.clear();
            for (auto stanza : out) {
                moved = true;
                if (qEnvironmentVariableIsSet("SXE_TEST_DUMP")) {
                    QString     xml;
                    QTextStream stream(&xml);
                    stanza.save(stream, 1);
                    qInfo("%s -> %s", qPrintable(from->jid.node()), qPrintable(xml));
                }
                if (stanza.tagName() == QLatin1String("message"))
                    sentMessages.append(stanza);
                stanza.setAttribute(QStringLiteral("from"), from->jid.full());
                stanza.setAttribute(QStringLiteral("to"), to->jid.full());
                to->client.rootTask()->take(stanza);
            }
        }
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        if (!moved && a.stream.written.isEmpty() && b.stream.written.isEmpty())
            return;
    }
    qFatal("stanza exchange did not settle");
}

static QString dump(const QDomDocument &doc)
{
    QString     out;
    QTextStream stream(&out);
    doc.documentElement().save(stream, -1);
    return out;
}

static QDomElement parse(QDomDocument &doc, const QString &text)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    const bool parsed = doc.setContent(text, true);
#else
    const auto parsed = doc.setContent(text, QDomDocument::ParseOption::UseNamespaceProcessing);
#endif
    check(bool(parsed), "invalid test XML");
    return doc.documentElement();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));

    check(alice.client.jingleManager()->discoFeatures().contains(SXE::NS_TRANSPORT)
              && alice.client.jingleManager()->discoFeatures().contains(SXE::NS)
              && alice.client.jingleManager()->discoFeatures().contains(SXE::NS_SVG_WHITEBOARD),
          "SXE features are not advertised");

    QPointer<J::Session> bobSession;
    QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &app,
                     [&](J::Session *session) { bobSession = session; });

    auto session = alice.client.jingleManager()->newSession(bob.jid);
    auto content = qobject_cast<SXE::Application *>(session->newContent(SXE::NS_SVG_WHITEBOARD, J::Origin::Both));
    check(content && content->isHost(), "whiteboard content was not created");
    QDomDocument initial;
    parse(initial,
          QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
                         "<path id='a' d='M 1 1 L 2 2'/></svg>"));
    content->setInitialDocument(initial);
    session->addContent(content);
    session->initiate();
    pump(alice, bob);

    check(bobSession && bobSession->preferredApplication() == SXE::NS_SVG_WHITEBOARD,
          "incoming whiteboard session not offered");
    auto remote = qobject_cast<SXE::Application *>(
        bobSession->contentList().value(J::ContentKey { content->contentName(), J::Origin::Initiator }));
    check(remote && !remote->isHost(), "incoming whiteboard content missing");
    auto transport = qobject_cast<SXE::Transport *>(remote->transport().data());
    check(transport && transport->hosts().value(0).compare(alice.jid), "SXE host is not the initiator");

    // Edits of the host before the joiner synchronizes become part of the snapshot.
    QDomDocument temp;
    content->document()->insertNodeAfter(
        parse(temp, QStringLiteral("<circle xmlns='http://www.w3.org/2000/svg' id='c' r='5'/>")),
        content->document()->document().documentElement());
    content->document()->flush();

    bobSession->accept();
    pump(alice, bob);
    check(content->state() == J::State::Active && remote->state() == J::State::Active, "SXE contents are not active");
    check(content->syncState() == SXE::Application::SyncState::Synchronized
              && remote->syncState() == SXE::Application::SyncState::Synchronized,
          "state synchronization did not complete");
    check(dump(remote->document()->document()) == dump(content->document()->document()),
          "joiner did not receive the hosted document");

    // Both directions edit; a conflict on the same record converges.
    auto hostPath = content->document()->document().documentElement().firstChildElement(QStringLiteral("path"));
    auto peerPath = remote->document()->document().documentElement().firstChildElement(QStringLiteral("path"));
    content->document()->setAttribute(hostPath, QStringLiteral("stroke"), QStringLiteral("red"));
    content->document()->flush();
    pump(alice, bob);
    check(peerPath.attribute(QStringLiteral("stroke")) == QStringLiteral("red"), "host edit not delivered");
    remote->document()->setAttribute(peerPath, QStringLiteral("d"), QStringLiteral("M 0 0"));
    remote->document()->flush();
    pump(alice, bob);
    check(hostPath.attribute(QStringLiteral("d")) == QStringLiteral("M 0 0"), "joiner edit not delivered");

    content->document()->setAttribute(hostPath, QStringLiteral("stroke"), QStringLiteral("green"));
    remote->document()->setAttribute(peerPath, QStringLiteral("stroke"), QStringLiteral("blue"));
    content->document()->flush();
    remote->document()->flush();
    pump(alice, bob);
    check(dump(remote->document()->document()) == dump(content->document()->document()),
          "conflicting edits diverged across the session");

    // Large embedded data is split over several stanzas and reassembled.
    sentMessages.clear();
    const QString image = QStringLiteral("data:image/png;base64,") + QString(40000, 'B');
    content->document()->setAttribute(hostPath, QStringLiteral("href"), image);
    content->document()->flush();
    pump(alice, bob);
    check(sentMessages.size() > 1, "large state was not split into several messages");
    check(peerPath.attribute(QStringLiteral("href")) == image, "large state was not reassembled");
    for (const auto &message : std::as_const(sentMessages)) {
        check(!message.firstChildElement(QStringLiteral("sxe")).isNull(), "SXE message without payload");
        check(message.firstChildElement(QStringLiteral("sxe")).namespaceURI() == SXE::NS, "wrong SXE namespace");
        check(!message.firstChildElement(QStringLiteral("no-store")).isNull(), "SXE state may be archived");
    }

    // Senders of an SXE content are the participants allowed to edit.
    check(content->supportsContentModify() && remote->supportsContentModify(), "SXE content cannot change senders");
    const auto frozenAt = dump(content->document()->document());
    check(content->requestSenders(J::Origin::None), "freezing the whiteboard was refused");
    pump(alice, bob);
    check(content->senders() == J::Origin::None && remote->senders() == J::Origin::None,
          "senders change was not negotiated");
    check(content->document()->isReadOnly() && remote->document()->isReadOnly(), "frozen document stays editable");
    sentMessages.clear();
    content->document()->setAttribute(hostPath, QStringLiteral("stroke"), QStringLiteral("frozen"));
    remote->document()->setAttribute(peerPath, QStringLiteral("stroke"), QStringLiteral("frozen"));
    content->document()->flush();
    remote->document()->flush();
    pump(alice, bob);
    check(sentMessages.isEmpty() && dump(content->document()->document()) == frozenAt
              && dump(remote->document()->document()) == frozenAt,
          "a frozen whiteboard was edited");

    // Only the initiator draws; edits of the responder are refused locally and ignored remotely.
    check(remote->requestSenders(J::Origin::Initiator), "presenter mode was refused");
    pump(alice, bob);
    check(content->senders() == J::Origin::Initiator && !content->document()->isReadOnly()
              && remote->document()->isReadOnly(),
          "presenter mode was not applied");
    {
        QDomDocument forged;
        auto         message = forged.createElement(QStringLiteral("message"));
        message.setAttribute(QStringLiteral("from"), bob.jid.full());
        message.setAttribute(QStringLiteral("to"), alice.jid.full());
        auto sxe = forged.createElementNS(SXE::NS, QStringLiteral("sxe"));
        sxe.setAttribute(QStringLiteral("session"), session->sid());
        sxe.setAttribute(QStringLiteral("id"), QStringLiteral("forged"));
        auto state = forged.createElementNS(SXE::NS, QStringLiteral("state"));
        auto edit  = forged.createElementNS(SXE::NS, QStringLiteral("new"));
        edit.setAttribute(QStringLiteral("rid"), QStringLiteral("forged.1"));
        edit.setAttribute(QStringLiteral("type"), QStringLiteral("element"));
        edit.setAttribute(QStringLiteral("name"), QStringLiteral("rect"));
        state.appendChild(edit);
        sxe.appendChild(state);
        message.appendChild(sxe);
        const auto before = dump(content->document()->document());
        // A valid parent: the first <new/> of the host snapshot is the root element.
        QDomDocument snapshotWire;
        edit.setAttribute(QStringLiteral("parent"),
                          content->document()->snapshot(snapshotWire).value(0).attribute(QStringLiteral("rid")));
        alice.client.rootTask()->take(message);
        check(dump(content->document()->document()) == before, "edit of a non-sender was applied");
    }
    content->document()->setAttribute(hostPath, QStringLiteral("stroke"), QStringLiteral("presenter"));
    content->document()->flush();
    pump(alice, bob);
    check(peerPath.attribute(QStringLiteral("stroke")) == QStringLiteral("presenter"), "presenter edit not delivered");

    // Resuming continues with the same records and versions, without a new state transfer.
    check(content->requestSenders(J::Origin::Both), "resuming the whiteboard was refused");
    pump(alice, bob);
    remote->document()->setAttribute(peerPath, QStringLiteral("stroke"), QStringLiteral("resumed"));
    remote->document()->flush();
    pump(alice, bob);
    check(hostPath.attribute(QStringLiteral("stroke")) == QStringLiteral("resumed")
              && dump(remote->document()->document()) == dump(content->document()->document()),
          "resumed whiteboard diverged");

    // XEP-0353 proposals carry the whiteboard description.
    {
        alice.client.jingleManager()->setMessageInitiationEnabled(true);
        bob.client.jingleManager()->setMessageInitiationEnabled(true);
        std::optional<J::MessageInitiation> received;
        QObject::connect(bob.client.jingleManager(), &J::Manager::incomingMessageInitiation, &app,
                         [&](const Message &, const J::MessageInitiation &initiation) { received = initiation; });
        J::MessageInitiation propose(J::MessageInitiation::Action::Propose, QStringLiteral("wb-proposal"));
        propose.addDescription(SXE::NS_SVG_WHITEBOARD, std::any(SXE::Proposal {}));
        check(alice.client.jingleManager()->sendMessageInitiation(Jid(bob.jid.bare()), propose),
              "whiteboard proposal was not sent");
        pump(alice, bob);
        check(received && received->action() == J::MessageInitiation::Action::Propose
                  && received->descriptions().size() == 1 && received->descriptions().first().isSupported()
                  && received->descriptions().first().data.type() == typeid(SXE::Proposal),
              "whiteboard proposal was not understood");
    }

    // SXE payload of a finished session is not routed anymore.
    session->terminate(J::Reason::Success);
    pump(alice, bob);
    check(!bobSession || bobSession->state() == J::State::Finished, "peer session not terminated");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    qInfo("SXE session regressions passed");
    return 0;
}
