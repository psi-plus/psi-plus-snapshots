// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDomDocument>
#include <iris/xmpp_message.h>
#include <iris/xmpp_stanza.h>

using namespace XMPP;

static void check(bool ok, const char *why)
{
    if (!ok)
        qFatal("%s", why);
}

static Message parse(const QString &inner)
{
    QDomDocument doc;
    const QString xml = QStringLiteral("<message xmlns='jabber:client' from='peer@example.test/phone' "
                                       "id='incoming-1' type='chat'>")
        + inner + QStringLiteral("</message>");
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    check(bool(doc.setContent(xml, QDomDocument::ParseOption::UseNamespaceProcessing)), "could not parse input");
#else
    check(doc.setContent(xml, true), "could not parse input");
#endif
    Message message;
    check(message.fromStanza(Stanza::fromXml(doc.documentElement())), "could not decode message stanza");
    return message;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto markable = parse(QStringLiteral("<body>hello</body>"
                                              "<markable xmlns='urn:xmpp:chat-markers:0'/>"
                                              "<request xmlns='urn:xmpp:receipts'/>"));
    check(markable.isMarkable(), "XEP-0333 markable flag lost");
    check(markable.displayedMarkerId().isEmpty(), "unexpected displayed marker");
    check(markable.messageReceipt() == ReceiptRequest, "XEP-0184 receipt request lost");

    const auto displayed = parse(QStringLiteral("<displayed xmlns='urn:xmpp:chat-markers:0' id='outgoing-1'/>"));
    check(!displayed.isMarkable(), "a displayed marker is not a markable message");
    check(displayed.displayedMarkerId() == QStringLiteral("outgoing-1"), "displayed id lost");
    check(displayed.messageReceipt() == ReceiptNone, "displayed marker became a delivery receipt");

    const auto missingId = parse(QStringLiteral("<displayed xmlns='urn:xmpp:chat-markers:0'/>"));
    check(missingId.displayedMarkerId().isEmpty(), "malformed displayed marker was accepted");
    const auto foreign = parse(QStringLiteral("<displayed xmlns='urn:other' id='bad'/>"
                                             "<markable xmlns='urn:other'/>"));
    check(!foreign.isMarkable() && foreign.displayedMarkerId().isEmpty(), "foreign marker namespace accepted");

    Message outbound(Jid(QStringLiteral("peer@example.test")));
    outbound.setId(QStringLiteral("new-outgoing"));
    outbound.setMarkable(true);
    outbound.setDisplayedMarkerId(QStringLiteral("previous"));
    check(outbound.isMarkable() && outbound.displayedMarkerId() == QStringLiteral("previous"),
          "outgoing marker state not retained");
    return 0;
}
