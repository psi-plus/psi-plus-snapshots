/* SPDX-License-Identifier: LGPL-2.1-or-later */
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

    QDomDocument source;
    check(source.setContent(QStringLiteral(
        "<message xmlns='jabber:client' from='bob@example.test'>"
        "<body>&gt; _ &lt;</body>"
        "<unstyled xmlns='urn:xmpp:styling:0'/>"
        "</message>"), true), "Cannot create XEP-0393 stanza");
    Message message;
    check(message.fromStanza(endpoint.stream.createStanza(source.documentElement())),
          "Cannot parse XEP-0393 unstyled message");
    check(message.stylingDisabled(), "Unstyled flag was lost on parse");
    check(message.body() == QStringLiteral("> _ <"), "Unstyled body was changed");

    const Stanza outgoing = message.toStanza(&endpoint.stream);
    check(!outgoing.isNull(), "Cannot serialize unstyled message");
    check(outgoing.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:styling:0"),
                                                QStringLiteral("unstyled")).size() == 1,
          "Missing XEP-0393 marker on serialization");
    Message roundtrip;
    check(roundtrip.fromStanza(outgoing), "Cannot parse serialized unstyled message");
    check(roundtrip.stylingDisabled(), "Roundtrip lost unstyled flag");
    roundtrip.setStylingDisabled(false);
    check(outgoing.element().elementsByTagNameNS(QStringLiteral("urn:xmpp:styling:0"),
                                                 QStringLiteral("unstyled")).size() == 1,
          "Mutating shared Message changed earlier stanza");
    check(roundtrip.toStanza(&endpoint.stream).element()
              .elementsByTagNameNS(QStringLiteral("urn:xmpp:styling:0"), QStringLiteral("unstyled")).isEmpty(),
          "Resetting flag left unwanted marker");

    QDomDocument ordinary;
    check(ordinary.setContent(QStringLiteral(
        "<message xmlns='jabber:client'><body>*normal*</body></message>"), true),
          "Cannot create ordinary message");
    Message noMarker;
    check(noMarker.fromStanza(endpoint.stream.createStanza(ordinary.documentElement())),
          "Cannot parse ordinary message");
    check(!noMarker.stylingDisabled(), "Invented an unstyled marker");
    return 0;
}
