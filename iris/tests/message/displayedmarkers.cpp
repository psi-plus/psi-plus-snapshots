// SPDX-License-Identifier: LGPL-2.1-or-later
// Ensure both halves of XEP-0333 are serialized independently of XEP-0184.
#include "fixture.h"
#include <iris/xmpp_message.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

static void expectSingle(const QDomElement &root, const QString &tag, int count)
{
    check(root.elementsByTagNameNS(QStringLiteral("urn:xmpp:chat-markers:0"), tag).count() == count,
          "Unexpected displayed-marker element count");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint endpoint;

    Message outbound(Jid(QStringLiteral("bob@example.test")));
    outbound.setType(Message::Type::Chat);
    outbound.setId(QStringLiteral("outbound"));
    outbound.setBody(QStringLiteral("Hello"));
    outbound.setMarkable(true);
    outbound.setMessageReceipt(ReceiptRequest);
    auto wire = outbound.toStanza(&endpoint.stream);
    check(!wire.isNull(), "Cannot serialize markable message");
    expectSingle(wire.element(), QStringLiteral("markable"), 1);
    expectSingle(wire.element(), QStringLiteral("displayed"), 0);
    check(wire.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:receipts"),
                                            QStringLiteral("request")).count() == 1,
          "markable message lost XEP-0184 receipt request");

    Message parsed;
    check(parsed.fromStanza(wire), "Could not parse the encoded markable message");
    check(parsed.isMarkable(), "Markable flag lost on roundtrip");
    check(parsed.messageReceipt() == ReceiptRequest, "Receipt request lost on roundtrip");

    Message marker(Jid(QStringLiteral("bob@example.test")));
    marker.setType(Message::Type::Chat);
    marker.setId(QStringLiteral("cursor-1"));
    marker.setDisplayedMarkerId(QStringLiteral("outbound"));
    wire = marker.toStanza(&endpoint.stream);
    check(!wire.isNull(), "Cannot serialize displayed marker");
    expectSingle(wire.element(), QStringLiteral("markable"), 0);
    expectSingle(wire.element(), QStringLiteral("displayed"), 1);
    const auto element
        = wire.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:chat-markers:0"),
                                              QStringLiteral("displayed")).item(0).toElement();
    check(element.attribute(QStringLiteral("id")) == QStringLiteral("outbound"), "Displayed id was wrong");
    check(wire.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:receipts"),
                                            QStringLiteral("received")).isEmpty(),
          "Displayed marker was incorrectly encoded as a delivery receipt");
    Message parsedMarker;
    check(parsedMarker.fromStanza(wire), "Could not parse encoded displayed marker");
    check(parsedMarker.displayedMarkerId() == QStringLiteral("outbound"), "Displayed id lost on roundtrip");

    // A request without its own stanza ID would be impossible to acknowledge.
    Message withoutId(Jid(QStringLiteral("bob@example.test")));
    withoutId.setMarkable(true);
    wire = withoutId.toStanza(&endpoint.stream);
    check(!wire.isNull(), "Could not serialize message lacking id");
    expectSingle(wire.element(), QStringLiteral("markable"), 0);
    return 0;
}
