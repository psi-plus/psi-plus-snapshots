// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0166: content-remove/reject carries one reason AFTER all contents.
#include <QCoreApplication>
#include <QDomDocument>

#include "../../src/xmpp/xmpp-im/jingle-update-xml_p.h"

using namespace XMPP::Jingle;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static QDomElement content(QDomDocument &doc, const QString &name)
{
    auto element = doc.createElementNS(NS, QStringLiteral("content"));
    element.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
    element.setAttribute(QStringLiteral("name"), name);
    return element;
}

static QDomElement reason(QDomDocument &doc, const QString &message)
{
    auto element = doc.createElementNS(NS, QStringLiteral("reason"));
    element.appendChild(doc.createElementNS(NS, QStringLiteral("failed-application")));
    auto text = doc.createElementNS(NS, QStringLiteral("text"));
    text.appendChild(doc.createTextNode(message));
    element.appendChild(text);
    return element;
}

static void testBatch(Action action)
{
    QDomDocument doc;
    auto jingle = doc.createElementNS(NS, QStringLiteral("jingle"));
    doc.appendChild(jingle);
    // Reproduce an audio and video teardown: each app contributes its own reason.
    const QList<QDomElement> updates {
        content(doc, QStringLiteral("video")), reason(doc, QStringLiteral("video failure")),
        content(doc, QStringLiteral("audio")), reason(doc, QStringLiteral("audio teardown failure"))
    };
    check(appendJingleActionUpdates(jingle, action, updates) == 1,
          "extra reason was not recognized");
    const auto children = jingle.childNodes();
    check(children.count() == 3, "batched removal emitted duplicate reasons");
    check(children.at(0).toElement().attribute(QStringLiteral("name")) == QLatin1String("video"),
          "first content missing or moved");
    check(children.at(1).toElement().attribute(QStringLiteral("name")) == QLatin1String("audio"),
          "second content missing or moved");
    check(children.at(2).toElement().localName() == QLatin1String("reason"),
          "reason must follow all content elements");
    check(jingle.elementsByTagNameNS(NS, QStringLiteral("reason")).count() == 1,
          "multiple Jingle reasons emitted");
    check(children.at(2).toElement().firstChildElement(QStringLiteral("text")).text()
              == QLatin1String("video failure"),
          "first removal reason was lost");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testBatch(Action::ContentRemove);
    testBatch(Action::ContentReject);

    QDomDocument doc;
    auto emptyReasonJingle = doc.createElementNS(NS, QStringLiteral("jingle"));
    const QList<QDomElement> updates {
        content(doc, QStringLiteral("audio")), content(doc, QStringLiteral("video"))
    };
    check(appendJingleActionUpdates(emptyReasonJingle, Action::ContentRemove, updates) == 0,
          "spurious extra reason reported");
    check(emptyReasonJingle.childNodes().count() == 2, "content removed when no reason was present");

    auto originalOrder = doc.createElementNS(NS, QStringLiteral("jingle"));
    check(appendJingleActionUpdates(originalOrder, Action::SessionAccept,
                                    { content(doc, QStringLiteral("audio")), reason(doc, QStringLiteral("reason")) }) == 0,
          "non-removal action changed");
    check(originalOrder.childNodes().count() == 2
              && originalOrder.lastChild().toElement().localName() == QLatin1String("reason"),
          "unrelated action was reordered");
    return 0;
}
