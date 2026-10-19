// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0421: parse occupant IDs without conflating them with trusted identities.
#include "fixture.h"
#include <iris/xmpp_message.h>
#include <iris/xmpp_status.h>
#include <iris/xmpp_tasks.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

static QDomElement parse(const QString &xml, QDomDocument &document)
{
    check(document.setContent(xml, true), "Invalid occupant-id fixture");
    return document.documentElement();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint endpoint;
    auto &stream = endpoint.stream;
    auto &client = endpoint.client;

    const auto parseMessage = [&](const QString &payload) {
        QDomDocument document;
        const auto element = parse(QStringLiteral(
            "<message xmlns='jabber:client' from='room@conference.example.test/alice' "
            "type='groupchat'><body>Hello</body>%1</message>").arg(payload), document);
        Message message;
        check(message.fromStanza(stream.createStanza(element)), "Cannot parse occupant message");
        return message;
    };
    const auto valid = QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='stable-id'/>");
    const auto message = parseMessage(valid);
    check(message.occupantId() == QStringLiteral("stable-id"), "Message occupant ID lost");
    const auto outgoing = message.toStanza(&stream);
    check(!outgoing.isNull(), "Cannot serialize occupant ID");
    const auto serialized = outgoing.element().elementsByTagNameNS(
        QStringLiteral("urn:xmpp:occupant-id:0"), QStringLiteral("occupant-id"));
    check(serialized.size() == 1 && serialized.at(0).toElement().attribute(QStringLiteral("id"))
          == QStringLiteral("stable-id"), "Message occupant ID not preserved");

    check(parseMessage(valid + valid).occupantId().isEmpty(), "Duplicate occupant IDs were accepted");
    check(parseMessage(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id=''/>")).occupantId().isEmpty(),
        "Empty occupant ID was accepted");
    check(parseMessage(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='%1'/>")
        .arg(QString(129, QLatin1Char('x')))).occupantId().isEmpty(),
        "Oversized occupant ID was accepted");
    // Astral characters occupy two UTF-16 code units but one Unicode character.
    const auto emoji = QStringLiteral("😀");
    const auto emoji128 = emoji.repeated(128);
    const auto emoji129 = emoji.repeated(129);
    check(parseMessage(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='%1'/>")
        .arg(emoji128)).occupantId() == emoji128,
        "128 non-BMP Unicode characters were incorrectly rejected");
    check(parseMessage(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='%1'/>")
        .arg(emoji129)).occupantId().isEmpty(),
        "129 non-BMP Unicode characters were accepted");
    check(parseMessage(QStringLiteral(
        "<occupant-id xmlns='urn:wrong:occupant-id' id='forged'/>")).occupantId().isEmpty(),
        "Incorrect namespace was accepted");
    check(parseMessage(QStringLiteral(
        "<o:occupant-id xmlns:o='urn:xmpp:occupant-id:0' id='prefixed'/>")).occupantId()
          == QStringLiteral("prefixed"), "Prefixed occupant ID was ignored");

    JT_PushPresence presence(client.rootTask());
    int received = 0;
    QString occupantId;
    QObject::connect(&presence, &JT_PushPresence::presence, &presence,
                     [&](const Jid &, const Status &status) {
                         ++received;
                         occupantId = status.occupantId();
                     });
    const auto parsePresence = [&](const QString &payload) {
        QDomDocument document;
        const auto element = parse(QStringLiteral(
            "<presence from='room@conference.example.test/alice'>%1</presence>").arg(payload), document);
        check(presence.take(element), "Cannot parse occupant presence");
    };
    parsePresence(valid);
    check(received == 1 && occupantId == QStringLiteral("stable-id"),
          "Presence occupant ID lost");
    parsePresence(valid + valid);
    check(received == 2 && occupantId.isEmpty(), "Duplicate presence occupant IDs were accepted");
    parsePresence(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='%1'/>")
        .arg(QString(129, QLatin1Char('x'))));
    check(received == 3 && occupantId.isEmpty(), "Oversized presence occupant ID was accepted");
    parsePresence(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='%1'/>").arg(emoji128));
    check(received == 4 && occupantId == emoji128,
          "128 non-BMP presence Unicode characters were incorrectly rejected");
    parsePresence(QStringLiteral(
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='%1'/>").arg(emoji129));
    check(received == 5 && occupantId.isEmpty(),
          "129 non-BMP presence Unicode characters were accepted");
    parsePresence(QStringLiteral(
        "<o:occupant-id xmlns:o='urn:xmpp:occupant-id:0' id='prefixed'/>"));
    check(received == 6 && occupantId == QStringLiteral("prefixed"),
          "Prefixed presence occupant ID was ignored");

    // These accessors expose UNTRUSTED data: room disco#info must advertise
    // urn:xmpp:occupant-id:0 before an application uses it as identity.
    return 0;
}
