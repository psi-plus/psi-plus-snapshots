/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <iris/xmpp_vcard4.h>

#include <QCoreApplication>
#include <QDomDocument>

using namespace XMPP::VCard4;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    VCard card;
    card.setFullName(QStringLiteral("Example"));
    card.setLanguages(QStringLiteral("en-US"));

    QDomDocument document;
    const auto element = card.toXmlElement(document);
    const auto language = element.firstChildElement(QStringLiteral("lang"));
    check(!language.isNull(), "Language entry was not serialized");
    check(language.firstChildElement(QStringLiteral("language-tag")).text() == QStringLiteral("en-US"),
          "RFC 6351 language-tag was not serialized");
    check(language.firstChildElement(QStringLiteral("text")).isNull(),
          "Language was incorrectly serialized with a text element");

    const auto fn = element.firstChildElement(QStringLiteral("fn"));
    check(fn.firstChildElement(QStringLiteral("text")).text() == QStringLiteral("Example"),
          "Regular text fields changed during language-tag fix");

    const VCard restored(element);
    check(restored.languages().size() == 1 && restored.languages().first().data == QStringLiteral("en-US"),
          "Language did not survive vCard4 serialization round trip");
    check(restored.fullName().size() == 1 && restored.fullName().first().data == QStringLiteral("Example"),
          "Full name did not survive vCard4 serialization round trip");
    return 0;
}
