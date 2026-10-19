// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0461 reply metadata roundtrip.
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
    QDomDocument doc;
    check(doc.setContent(QStringLiteral(
        "<message xmlns='jabber:client' from='bob@example.test/phone' type='chat'>"
        "<body>Reply</body><reply xmlns='urn:xmpp:reply:0' id='previous-id' "
        "to='alice@example.test/desktop'/></message>"), true), "Invalid fixture");
    Message message;
    check(message.fromStanza(stream.createStanza(doc.documentElement())), "Reply parse failed");
    check(message.reply().isValid(), "Valid reply missing");
    check(message.reply().id == QStringLiteral("previous-id"), "Reply ID changed");
    check(message.reply().to.full() == QStringLiteral("alice@example.test/desktop"),
          "Reply author changed");

    const auto stanza = message.toStanza(&stream);
    check(!stanza.isNull(), "Reply serialization failed");
    Message roundtrip;
    check(roundtrip.fromStanza(stanza), "Reply roundtrip parse failed");
    check(roundtrip.reply().id == message.reply().id, "Reply roundtrip lost ID");

    QDomDocument optionalDoc;
    check(optionalDoc.setContent(QStringLiteral(
        "<message xmlns='jabber:client'><reply xmlns='urn:xmpp:reply:0' id='room-id'/>"
        "</message>"), true), "Invalid optional-to fixture");
    Message optional;
    check(optional.fromStanza(stream.createStanza(optionalDoc.documentElement())),
          "Optional-to parse failed");
    check(optional.reply().isValid() && optional.reply().to.isEmpty(),
          "Optional reply author was treated as required");

    Message invalid;
    invalid.setReply({ Jid(), QString() });
    check(!invalid.reply().isValid(), "Missing reply ID accepted");
    return 0;
}
