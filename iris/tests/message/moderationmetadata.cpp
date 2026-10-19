// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0425: moderated actions and MAM tombstones are different semantics.
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
    const auto parse = [&](const QString &from, const QString &payload) {
        QDomDocument doc;
        check(doc.setContent(QStringLiteral(
            "<message xmlns='jabber:client' type='groupchat' from='%1'>%2</message>")
            .arg(from, payload), true), "Invalid moderation XML fixture");
        Message msg;
        check(msg.fromStanza(endpoint.stream.createStanza(doc.documentElement())),
              "Cannot parse moderated message");
        return msg;
    };
    const QString room = QStringLiteral("room@conference.example.test");
    const QString occupant = room + QStringLiteral("/alice");
    const QString moderated = QStringLiteral(
        "<moderated xmlns='urn:xmpp:message-moderate:1' by='room@conference.example.test/mod'>"
        "<occupant-id xmlns='urn:xmpp:occupant-id:0' id='mod-uuid'/></moderated>");
    const QString actionXml = QStringLiteral(
        "<retract xmlns='urn:xmpp:message-retract:1' id='room-id-42'>%1"
        "<reason>Spam</reason></retract>").arg(moderated);
    auto action = parse(room, actionXml);
    check(action.retraction() == QStringLiteral("room-id-42"), "Ordinary retraction ID lost");
    auto mod = action.moderatedRetraction();
    check(mod.kind == Message::ModeratedRetraction::Kind::Action
              && mod.by.full() == room + QStringLiteral("/mod")
              && mod.moderatorOccupantId == QStringLiteral("mod-uuid")
              && mod.reason == QStringLiteral("Spam") && mod.stamp.isEmpty(),
          "Moderation action metadata lost");

    const auto untrusted = parse(occupant, actionXml);
    check(untrusted.moderatedRetraction().kind == Message::ModeratedRetraction::Kind::None
              && untrusted.retraction().isEmpty(),
          "MUC occupant spoofed a moderation action or ordinary retraction");

    const QString tombstoneXml = QStringLiteral(
        "<retracted xmlns='urn:xmpp:message-retract:1' stamp='2026-10-17T09:00:00Z'>"
        "%1<reason>Removed by moderator</reason></retracted>").arg(moderated);
    auto tombstone = parse(occupant, tombstoneXml);
    mod = tombstone.moderatedRetraction();
    check(mod.kind == Message::ModeratedRetraction::Kind::Tombstone
              && mod.reason == QStringLiteral("Removed by moderator")
              && mod.stamp == QStringLiteral("2026-10-17T09:00:00Z")
              && tombstone.retraction().isEmpty(),
          "Tombstone was lost or interpreted as an action");

    auto invalidStamp = parse(occupant,
        QStringLiteral("<retracted xmlns='urn:xmpp:message-retract:1' stamp='invalid'>%1</retracted>")
           .arg(moderated));
    check(invalidStamp.moderatedRetraction().kind == Message::ModeratedRetraction::Kind::None,
          "Malformed tombstone timestamp accepted");

    auto plain = parse(room, QStringLiteral(
        "<retract xmlns='urn:xmpp:message-retract:1' id='normal-retraction'/>"));
    check(plain.retraction() == QStringLiteral("normal-retraction")
              && plain.moderatedRetraction().kind == Message::ModeratedRetraction::Kind::None,
          "XEP-0424 plain retraction semantics changed");

    const auto unrelated = parse(room, QStringLiteral(
        "<retract xmlns='urn:xmpp:message-retract:1' id='ordinary'>"
        "<moderated xmlns='urn:xmpp:other:0'/></retract>"));
    check(unrelated.retraction() == QStringLiteral("ordinary")
              && unrelated.moderatedRetraction().kind == Message::ModeratedRetraction::Kind::None,
          "Unrelated XML namespace was mistaken for moderation metadata");
    return 0;
}
