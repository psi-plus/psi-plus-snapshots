// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0385 media sharing without an outer reference URI, and XEP-0428
// fallback ranges counted in code points (XEP-0426).
#include "fixture.h"
#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>
#include <iris/xmpp_message.h>
#include <iris/xmpp_reference.h>

using namespace XMPP;
using namespace MessageTest;

namespace {
Message parse(Endpoint &endpoint, const QString &xml)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "Invalid fixture");
    Message message;
    check(message.fromStanza(endpoint.stream.createStanza(doc.documentElement())), "Message parse failed");
    return message;
}

const QString url = QStringLiteral("https://upload.example.test/u/abc/whiteboard.png");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint         endpoint;

    // A real-world share: the outer reference has no 'uri' (as in every
    // XEP-0385 example) and the body URL is marked as XEP-0428 fallback.
    const QString body  = QStringLiteral("Переслано:\n> ") + url + QStringLiteral("\n\n");
    const int     start = 13, end = start + url.size();
    const auto    share
        = parse(endpoint,
                QStringLiteral(
                    "<message xmlns='jabber:client' from='bob@example.test/laptop' type='chat'>"
                    "<reference xmlns='urn:xmpp:reference:0' type='data'>"
                    "<media-sharing xmlns='urn:xmpp:sims:1'>"
                    "<file xmlns='urn:xmpp:jingle:apps:file-transfer:5'><name>whiteboard.png</name>"
                    "<hash xmlns='urn:xmpp:hashes:2' algo='sha-256'>XE3H4XMnuwh2xl+tqQ1JGT5Q8a5lsc4KA9GBmMtYD8U=</hash>"
                    "<media-type>image/png</media-type><size>57957</size></file>"
                    "<sources><reference xmlns='urn:xmpp:reference:0' type='data' uri='%1'/></sources>"
                    "</media-sharing></reference>"
                    "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:sims:1'><body start='%2' end='%3'/></fallback>"
                    "<fallback xmlns='urn:xmpp:fallback:0' for='jabber:x:oob'><body start='%2' end='%3'/></fallback>"
                    "<body>%4</body></message>")
                    .arg(url)
                    .arg(start)
                    .arg(end)
                    .arg(body.toHtmlEscaped()));
    check(share.references().size() == 1, "Media share without an outer URI was dropped");
    const auto reference = share.references().first();
    check(reference.uri().isEmpty() && reference.begin() == -1, "Outer reference attributes changed");
    check(reference.mediaSharing().isValid() && reference.mediaSharing().sources == QStringList { url },
          "Media share sources lost");

    const auto sims = share.bodyFallbacks(QStringLiteral("urn:xmpp:sims:1"));
    check(sims.size() == 1 && sims.first().start == start && sims.first().end == end, "SIMS fallback range lost");
    check(share.bodyFallbacks(QStringLiteral("jabber:x:oob")).size() == 1, "OOB fallback range lost");
    int from = -1, to = -1;
    check(sims.first().toUtf16(share.body(), from, to) && share.body().mid(from, to - from) == url,
          "Fallback range does not select the body URL");

    // A reference without media sharing still requires a URI (XEP-0372).
    const auto mention = parse(endpoint,
                               QStringLiteral("<message xmlns='jabber:client'><body>hi</body>"
                                              "<reference xmlns='urn:xmpp:reference:0' type='mention'/></message>"));
    check(mention.references().isEmpty(), "Reference without URI or media sharing accepted");

    // Serialization omits the empty URI and keeps the fallback ranges.
    const auto stanza = share.toStanza(&endpoint.stream);
    check(!stanza.isNull(), "Serialization failed");
    const auto outer = stanza.element().firstChildElement(QStringLiteral("reference"));
    check(!outer.isNull() && !outer.hasAttribute(QStringLiteral("uri")), "Empty reference URI serialized");
    Message roundtrip;
    check(roundtrip.fromStanza(stanza), "Roundtrip parse failed");
    check(roundtrip.references().size() == 1, "Roundtrip lost the media share");
    const auto again = roundtrip.bodyFallbacks(QStringLiteral("urn:xmpp:sims:1"));
    check(again.size() == 1 && again.first().start == start && again.first().end == end,
          "Roundtrip lost the fallback range");

    // XEP-0426: indices are code points, not UTF-16 units. U+1F600 is one
    // code point but two QChars, so "x" after it is code point 1.
    const QString          emoji = QString::fromUtf8("\xF0\x9F\x98\x80x");
    Message::FallbackRange range { 1, 2 };
    check(range.toUtf16(emoji, from, to) && from == 2 && to == 3, "Code point range mapped as UTF-16");
    Message::FallbackRange outside { 1, 5 };
    check(!outside.toUtf16(emoji, from, to), "Out of bounds range accepted");

    // A fallback without children covers every body and subject; a range
    // with only one index is malformed and ignored.
    const auto whole       = parse(endpoint,
                                   QStringLiteral("<message xmlns='jabber:client'><body>quoted</body>"
                                                        "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:reply:0'/>"
                                                        "<fallback xmlns='urn:xmpp:fallback:0' for='urn:xmpp:sims:1'>"
                                                        "<body start='2'/></fallback></message>"));
    const auto replyRanges = whole.bodyFallbacks(QStringLiteral("urn:xmpp:reply:0"));
    check(replyRanges.size() == 1 && replyRanges.first().isWhole(), "Childless fallback is not whole-body");
    check(replyRanges.first().toUtf16(whole.body(), from, to) && from == 0 && to == whole.body().size(),
          "Whole-body range does not cover the body");
    check(whole.bodyFallbacks(QStringLiteral("urn:xmpp:sims:1")).isEmpty(), "Half-open range accepted");
    return 0;
}
