// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0425: moderation IQ requires the room's own stanza ID.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp-im/xmpp_moderation.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;

static void check(bool ok, const char *msg) { if (!ok) qFatal("%s", msg); }

class OfflineConnector final : public Connector {
public:
    void setOptHostPort(const QString &, quint16) override { }
    void connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void done() override { }
};
class Recorder final : public ClientStream {
public:
    explicit Recorder(Connector *connector) : ClientStream(connector) { }
    void write(const Stanza &s) override { sent.append(s.element().cloneNode(true).toElement()); }
    QList<QDomElement> sent;
};
static QDomElement xml(const QString &text)
{
    QDomDocument doc;
    check(doc.setContent(text, true), "Invalid moderation XML fixture");
    return doc.documentElement();
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    OfflineConnector connector;
    Recorder stream(&connector);
    Client client;
    client.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(), QStringLiteral("desktop"));
    client.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));
    const Jid room(QStringLiteral("room@conference.example.test"));
    const Message::StanzaId roomId { room, QStringLiteral("room-stanza-42") };

    ModeratedRetractionTask invalid(client.rootTask());
    invalid.request(room, { Jid(QStringLiteral("alice@example.test")), QStringLiteral("wrong-by") });
    invalid.go();
    check(stream.sent.isEmpty() && !invalid.success(), "Untrusted assigning JID allowed moderation");

    ModeratedRetractionTask fullRoom(client.rootTask());
    fullRoom.request(Jid(QStringLiteral("room@conference.example.test/nick")), roomId);
    fullRoom.go();
    check(stream.sent.isEmpty() && !fullRoom.success(), "Full room JID was accepted");

    ModeratedRetractionTask emptyId(client.rootTask());
    emptyId.request(room, { room, QString() });
    emptyId.go();
    check(stream.sent.isEmpty() && !emptyId.success(), "Empty target stanza ID was accepted");

    ModeratedRetractionTask task(client.rootTask());
    int finished = 0;
    QObject::connect(&task, &Task::finished, &task, [&] { ++finished; });
    task.request(room, roomId, QStringLiteral("Spam"));
    task.go();
    check(stream.sent.size() == 1, "No moderation IQ sent");
    const auto iq = stream.sent.first();
    check(iq.tagName() == QStringLiteral("iq") && iq.attribute(QStringLiteral("to")) == room.bare()
              && iq.attribute(QStringLiteral("type")) == QStringLiteral("set"),
          "Moderation IQ envelope invalid");
    const auto moderate = iq.firstChildElement();
    check(moderate.namespaceURI() == QStringLiteral("urn:xmpp:message-moderate:1")
              && moderate.attribute(QStringLiteral("id")) == QStringLiteral("room-stanza-42"),
          "Moderation target is not the MUC-issued ID");
    const auto retract = moderate.firstChildElement();
    check(retract.namespaceURI() == QStringLiteral("urn:xmpp:message-retract:1")
              && retract.localName() == QStringLiteral("retract"), "Missing XEP-0424 retract");
    check(retract.nextSiblingElement().text() == QStringLiteral("Spam"), "Moderation reason lost");

    const auto spoofed = QStringLiteral("<iq type='result' from='attacker@example.test' id='%1'/>")
                              .arg(iq.attribute(QStringLiteral("id")));
    check(client.rootTask()->take(xml(spoofed)) == false && finished == 0,
          "Spoofed moderation acknowledgement was accepted");
    // iqVerify checks sender and ID, but inbound get/set requests can
    // independently reuse a pending response ID.
    for (const auto &type : { QStringLiteral("get"), QStringLiteral("set") }) {
        const auto request = QStringLiteral(
            "<iq type='%1' from='room@conference.example.test' id='%2'>"
            "<query xmlns='urn:example:unrelated'/></iq>")
            .arg(type, iq.attribute(QStringLiteral("id")));
        check(!client.rootTask()->take(xml(request)) && finished == 0,
              "Incoming IQ get/set was swallowed as a moderation response");
    }
    const auto ack = QStringLiteral("<iq type='result' from='room@conference.example.test' id='%1'/>")
                          .arg(iq.attribute(QStringLiteral("id")));
    check(client.rootTask()->take(xml(ack)), "MUC moderation result was not handled");
    check(task.success() && finished == 1, "Successful moderation acknowledgement failed");
    check(!client.rootTask()->take(xml(ack)) && finished == 1,
          "Completed moderation task consumed a duplicate acknowledgement");

    ModeratedRetractionTask denied(client.rootTask());
    denied.request(room, roomId);
    denied.go();
    check(stream.sent.size() == 2, "No second IQ for error test");
    const auto err = QStringLiteral(
        "<iq type='error' from='room@conference.example.test' id='%1'>"
        "<error type='auth'><forbidden xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/></error></iq>")
                         .arg(stream.sent.last().attribute(QStringLiteral("id")));
    check(client.rootTask()->take(xml(err)), "Moderation error was not consumed");
    check(!denied.success() && !denied.statusString().isEmpty(), "Moderation error was swallowed");

    ModeratedRetractionTask malformed(client.rootTask());
    int malformedFinished = 0;
    QObject::connect(&malformed, &Task::finished, &malformed, [&] { ++malformedFinished; });
    malformed.request(room, roomId);
    malformed.go();
    const auto missingErrorPayload = QStringLiteral(
        "<iq type='error' from='room@conference.example.test' id='%1'/>")
        .arg(stream.sent.last().attribute(QStringLiteral("id")));
    check(client.rootTask()->take(xml(missingErrorPayload)),
          "Correlated malformed moderation error was not handled");
    check(malformedFinished == 1 && !malformed.success() && !malformed.statusString().isEmpty(),
          "Moderation error without payload failed to terminate the task");
    return 0;
}
