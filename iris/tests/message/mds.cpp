// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0490: private PEP displayed-state codec and untrusted input validation.
#include "fixture.h"
#include <iris/xmpp_mds.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

namespace {
PubSubItem itemFromXml(const QString &chat, const QString &xml)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "Malformed test XML");
    return PubSubItem(chat, doc.documentElement());
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    const Jid account(QStringLiteral("juliet@capulet.test"));
    const Jid room(QStringLiteral("hall@conference.example.test"));
    const Jid contact(QStringLiteral("romeo@example.test"));
    const Message::StanzaId accountId { account, QStringLiteral("server-msg-7") };
    check(account.isBare() && !Jid(QStringLiteral("juliet@capulet.test/phone")).isBare()
              && !Jid().isBare() && accountId.isValid()
              && !(Message::StanzaId { Jid(), QStringLiteral("id") }).isValid(),
          "Bare JID or stanza ID validity is inconsistent");

    check(MessageDisplayedSync::node() == QStringLiteral("urn:xmpp:mds:displayed:0"),
          "Incorrect PEP node");
    const auto options = MessageDisplayedSync::publishOptions();
    check(options.size() == 4 && options.value(QStringLiteral("pubsub#persist_items"))
                    == QStringList { QStringLiteral("true") }
              && options.value(QStringLiteral("pubsub#max_items")) == QStringList { QStringLiteral("max") }
              && options.value(QStringLiteral("pubsub#send_last_published_item"))
                    == QStringList { QStringLiteral("never") }
              && options.value(QStringLiteral("pubsub#access_model"))
                    == QStringList { QStringLiteral("whitelist") }, "Unsafe XEP-0490 publish options");

    QDomDocument document;
    auto displayed = MessageDisplayedSync::payload(document, accountId);
    check(!displayed.isNull() && displayed.namespaceURI() == MessageDisplayedSync::node(),
          "Cannot serialize displayed payload");
    document.appendChild(displayed);
    auto sid = displayed.firstChildElement();
    check(sid.namespaceURI() == QStringLiteral("urn:xmpp:sid:0")
              && sid.attribute(QStringLiteral("by")) == account.bare()
              && sid.attribute(QStringLiteral("id")) == QStringLiteral("server-msg-7"),
          "Incorrect displayed stanza-id");
    check(MessageDisplayedSync::payload(document, { Jid(), QStringLiteral("id") }).isNull()
              && MessageDisplayedSync::payload(document, { account, QString() }).isNull()
              && MessageDisplayedSync::payload(document,
                    { Jid(QStringLiteral("juliet@capulet.test/desktop")), QStringLiteral("id") }).isNull(),
          "Invalid stanza-id was serialized");

    MessageDisplayedSync::State state;
    check(MessageDisplayedSync::parseItem(PubSubItem(contact.bare(), displayed), account, &state),
          "Valid PEP item was rejected");
    check(state.chat == contact && state.stanzaId.by == account
              && state.stanzaId.id == QStringLiteral("server-msg-7"),
          "Parsed displayed state is wrong");
    check(!MessageDisplayedSync::parseItem(PubSubItem(contact.bare(), displayed), room, &state),
          "Incorrect stanza-id issuer was accepted");
    check(!MessageDisplayedSync::parseItem(PubSubItem(contact.bare(), displayed), account, nullptr),
          "Null state output was accepted");

    const auto prefixed = itemFromXml(QStringLiteral("hall@conference.example.test"),
        QStringLiteral("<mds:displayed xmlns:mds='urn:xmpp:mds:displayed:0' "
                       "xmlns:sid='urn:xmpp:sid:0'><sid:stanza-id "
                       "by='hall@conference.example.test' id='room-42'/></mds:displayed>"));
    check(MessageDisplayedSync::parseItem(prefixed, room, &state)
              && state.chat == room && state.stanzaId.id == QStringLiteral("room-42"),
          "Namespace-prefixed MDS item failed");

    const auto pm = itemFromXml(QStringLiteral("hall@conference.example.test/alice"),
        QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'>"
                       "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test' id='private-9'/>"
                       "</displayed>"));
    check(MessageDisplayedSync::parseItem(pm, account, &state)
              && state.chat.resource() == QStringLiteral("alice"),
          "MUC private-message item ID was lost");

    const auto invalid = [&](const QString &chat, const QString &xml) {
        const auto oldId = state.stanzaId.id;
        check(!MessageDisplayedSync::parseItem(itemFromXml(chat, xml), account, &state),
              "Malformed MDS payload was accepted");
        check(state.stanzaId.id == oldId, "Failed parse mutated output");
    };
    const QString chat = QStringLiteral("romeo@example.test");
    invalid(chat, QStringLiteral("<displayed xmlns='urn:xmpp:other:0'>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test' id='one'/></displayed>"));
    invalid(chat, QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'/>"));
    invalid(chat, QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test' id='one'/>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test' id='two'/></displayed>"));
    invalid(chat, QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='attacker@example.test' id='one'/></displayed>"));
    invalid(chat, QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test/phone' id='one'/></displayed>"));
    invalid(chat, QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test'/></displayed>"));
    invalid(QStringLiteral("not a jid @"), QStringLiteral("<displayed xmlns='urn:xmpp:mds:displayed:0'>"
        "<stanza-id xmlns='urn:xmpp:sid:0' by='juliet@capulet.test' id='one'/></displayed>"));

    const PubSubEvent ownEvent(PubSubEvent::Type::Items, MessageDisplayedSync::node(), {}, {}, {});
    const PubSubEvent wrongNode(PubSubEvent::Type::Items, QStringLiteral("urn:other:0"), {}, {}, {});
    const PubSubEvent deleteEvent(PubSubEvent::Type::Delete, MessageDisplayedSync::node(), {}, {}, {});
    check(MessageDisplayedSync::isOwnNotification(account, Jid(QStringLiteral("juliet@capulet.test/phone")),
                                                  ownEvent), "Own bare sender rejected");
    check(!MessageDisplayedSync::isOwnNotification(
              Jid(QStringLiteral("juliet@capulet.test/other")), account, ownEvent),
          "Full-JID sender was trusted");
    check(!MessageDisplayedSync::isOwnNotification(Jid(QStringLiteral("attacker@example.test")), account,
                                                   ownEvent), "Third-party sender was trusted");
    check(!MessageDisplayedSync::isOwnNotification(account, account, wrongNode)
              && !MessageDisplayedSync::isOwnNotification(account, account, deleteEvent),
          "Unrelated PEP event was accepted");

    return 0;
}
