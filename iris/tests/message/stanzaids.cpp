// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0359: preserve multiple assigning entities and avoid ambiguous IDs.
#include "fixture.h"
#include <iris/xmpp_message.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint endpoint;
    auto &stream = endpoint.stream;

    QDomDocument source;
    check(source.setContent(QStringLiteral(
              "<message xmlns='jabber:client' from='room@conference.example.test/alice' "
              "to='alice@example.test/desktop' type='groupchat' id='m1'>"
              "<body>Hello</body>"
              "<origin-id xmlns='urn:xmpp:sid:0' id='client-origin'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='room-id' by='room@conference.example.test'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='account-id' by='alice@example.test'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='forged-duplicate' by='room@conference.example.test'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' id='missing-by'/>"
              "<stanza-id xmlns='urn:xmpp:sid:0' by='server.example.test'/>"
              "</message>"), true), "Invalid stanza ID fixture");

    Message message;
    check(message.fromStanza(stream.createStanza(source.documentElement())), "Cannot parse message");
    check(message.stanzaIds().size() == 2, "Distinct stanza IDs were lost or invalid IDs accepted");
    check(message.stanzaIds().at(0).isValid() && message.stanzaIds().at(1).isValid(),
          "Parsed stanza IDs failed their own validity check");
    check(message.stanzaId(Jid(QStringLiteral("room@conference.example.test"))).id
              == QStringLiteral("room-id"), "Room stanza ID was overwritten");
    check(message.stanzaId(Jid(QStringLiteral("alice@example.test"))).id
              == QStringLiteral("account-id"), "Account stanza ID was lost");
    check(message.stanzaId(Jid(QStringLiteral("untrusted.example.test"))).id.isEmpty(),
          "Unknown assigning entity returned an ID");
    check(message.stanzaIds().at(0).id == QStringLiteral("room-id")
              && message.stanzaIds().at(1).id == QStringLiteral("account-id"),
          "Stanza ID order was not retained");
    check(message.originId() == QStringLiteral("client-origin"), "Origin ID was lost");

    const auto outgoing = message.toStanza(&stream);
    check(!outgoing.isNull(), "Could not serialize stanza IDs");
    const auto serializedIds
        = outgoing.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:sid:0"),
                                                 QStringLiteral("stanza-id"));
    check(serializedIds.size() == 2, "Serialization did not preserve both stanza IDs");

    Message roundTrip;
    check(roundTrip.fromStanza(outgoing), "Cannot parse serialized stanza");
    check(roundTrip.stanzaIds().size() == 2, "Roundtrip lost a stanza ID");
    check(roundTrip.stanzaId(Jid(QStringLiteral("room@conference.example.test"))).id
              == QStringLiteral("room-id"), "Roundtrip changed the room ID");

    roundTrip.addStanzaId({ Jid(QStringLiteral("alice@example.test")), QStringLiteral("duplicate") });
    check(roundTrip.stanzaIds().size() == 2, "Duplicate assigning entity was accepted");
    roundTrip.setStanzaIds({
        { Jid(QStringLiteral("new.example.test")), QStringLiteral("replacement") },
        { Jid(QStringLiteral("new.example.test")), QStringLiteral("duplicate") },
        { Jid(QStringLiteral("another.example.test")), QStringLiteral("another") }
    });
    check(roundTrip.stanzaIds().size() == 2
              && roundTrip.stanzaId(Jid(QStringLiteral("new.example.test"))).id == QStringLiteral("replacement")
              && roundTrip.stanzaId(Jid(QStringLiteral("another.example.test"))).id == QStringLiteral("another"),
          "setStanzaIds() did not replace and validate the list");

    // Ordinary stanza IDs and XEP-0359 origin IDs are independent.
    QDomDocument noOriginDoc;
    check(noOriginDoc.setContent(QStringLiteral(
              "<message xmlns='jabber:client' from='bob@example.test' id='legacy-id'>"
              "<body>Legacy message</body></message>"), true), "Invalid no-origin fixture");
    Message noOrigin;
    check(noOrigin.fromStanza(stream.createStanza(noOriginDoc.documentElement())),
          "Cannot parse a message without origin-id");
    check(noOrigin.id() == QStringLiteral("legacy-id") && noOrigin.originId().isEmpty(),
          "Incoming stanza id was incorrectly promoted to origin-id");
    const auto noOriginWire = noOrigin.toStanza(&stream);
    check(!noOriginWire.isNull(), "Cannot serialize message without origin-id");
    check(noOriginWire.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:sid:0"),
                                                     QStringLiteral("origin-id")).size() == 0,
          "Roundtrip fabricated an XEP-0359 origin-id");

    Message locallyComposed;
    locallyComposed.setId(QStringLiteral("outgoing-id"));
    check(locallyComposed.originId() == QStringLiteral("outgoing-id"),
          "Locally composed messages lost automatic origin-id generation");
    return 0;
}
