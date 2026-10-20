/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <iris/xmpp_bookmarks.h>
#include <iris/xmpp_blocking.h>
#include <iris/xmpp_pubsubitem.h>
#include "../../src/xmpp/xmpp-im/xmpp_pubsubreplica.h"

#include <QCoreApplication>
#include <QDomDocument>

using namespace XMPP;

static void check(bool ok, const char *reason)
{
    if (!ok)
        qFatal("%s", reason);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QDomDocument source;
    check(source.setContent(QStringLiteral(
        "<conference xmlns='urn:xmpp:bookmarks:1' name='Room' autojoin='true'>"
        "<nick>me</nick><password>secret</password><extensions>"
        "<state xmlns='urn:example:client-ext' favorite='yes'/></extensions></conference>"), true),
        "Cannot construct native bookmarks fixture");
    NativeBookmark original = NativeBookmark::fromItem(
        PubSubItem(QStringLiteral("room@conference.example.test"), source.documentElement()));
    check(original.isValid(), "XEP-0402 bookmark was rejected");
    check(original.autoJoin(), "autojoin lost");
    check(original.nick() == QStringLiteral("me"), "nick lost");
    check(original.password() == QStringLiteral("secret"), "password lost");

    // Editing a value copy must not mutate the original bookmark or its
    // opaque extension subtree. Serialization is a pure export operation.
    const NativeBookmark unchanged = original;
    original.setName(QStringLiteral("New room name"));
    original.setNick(QStringLiteral("new nickname"));
    original.setAutoJoin(false);
    check(unchanged.name() == QStringLiteral("Room"), "Editing value copy changed old name");
    check(unchanged.nick() == QStringLiteral("me"), "Editing value copy changed old nickname");
    check(unchanged.autoJoin(), "Editing value copy changed old autojoin");
    QDomDocument roundtrip;
    const auto updated = original.toItem(roundtrip);
    check(updated.id() == QStringLiteral("room@conference.example.test"), "room item id changed");
    check(updated.payload().elementsByTagNameNS(QStringLiteral("urn:example:client-ext"),
                                                 QStringLiteral("state")).size() == 1,
          "Unknown extensions were dropped on republish");
    check(updated.payload().attribute(QStringLiteral("name")) == QStringLiteral("New room name"),
          "Updated name lost");
    check(NativeBookmark::fromItem(updated).nick() == QStringLiteral("new nickname"),
          "Updated nickname lost");
    check(!NativeBookmark::fromItem(updated).autoJoin(), "Updated autojoin lost");

    QDomDocument previousDoc;
    const auto previous = unchanged.toItem(previousDoc);
    check(previous.payload().attribute(QStringLiteral("name")) == QStringLiteral("Room"),
          "Serializing edited copy changed original markup");
    check(previous.payload().attribute(QStringLiteral("autojoin")) == QStringLiteral("true"),
          "Serializing edited copy changed original autojoin");
    check(previous.payload().elementsByTagNameNS(QStringLiteral("urn:example:client-ext"),
                                                 QStringLiteral("state")).size() == 1,
          "Opaque extensions were not preserved when copying a bookmark");

    NativeBookmark withoutOptionals = NativeBookmark::create(
        Jid(QStringLiteral("simple@conference.example.test")), QStringLiteral("Simple"), false);
    QDomDocument minimalDoc;
    const auto minimal = withoutOptionals.toItem(minimalDoc);
    check(minimal.payload().firstChildElement().isNull(), "Unset fields produced empty XML children");
    check(minimal.payload().attribute(QStringLiteral("autojoin")) == QStringLiteral("false"),
          "Native bookmark autojoin false was not serialized");

    // First real consumer: an owned NativeBookmark can be buffered while the
    // PubSub IQ is in flight, modified by key and exported only for publish.
    PubSubReplica<NativeBookmark> replica;
    const auto fetchToken = replica.beginLoad();
    replica.published(unchanged.room().bare(), unchanged);
    check(replica.finishLoad(fetchToken, {}), "Could not merge bookmark event with empty snapshot");
    auto renamed = replica.items().value(unchanged.room().bare());
    check(renamed.isValid(), "PEP event lost during bookmark load");
    renamed.setName(QStringLiteral("Renamed from replica"));
    replica.put(renamed.room().bare(), renamed);
    auto batch = replica.beginWrite();
    check(batch.changes.size() == 1 && batch.changes.value(renamed.room().bare()).has_value(),
          "Native bookmark intent not generated");
    check(replica.finishWrite(batch, true), "Native bookmark write not acknowledged");
    QDomDocument replicaDoc;
    const auto replicaItem = replica.items().value(renamed.room().bare()).toItem(replicaDoc);
    check(replicaItem.payload().attribute(QStringLiteral("name")) == QStringLiteral("Renamed from replica"),
          "Native bookmark value was not updated");
    check(replicaItem.payload().elementsByTagNameNS(QStringLiteral("urn:example:client-ext"),
                                                    QStringLiteral("state")).size() == 1,
          "PEP reconciliation stripped native bookmark extensions");

    // An extension-only update must count as a changed item even when all
    // standard fields are identical. This is important for PEP pushes.
    NativeBookmark withoutExtension = NativeBookmark::create(
        original.room(), original.name(), original.autoJoin(), original.nick(), original.password());
    check(!original.hasSamePayload(withoutExtension),
          "XEP-0402 extension-only change was invisible to comparison");
    check(original.hasSamePayload(unchanged) == false,
          "Edited bookmark unexpectedly matched its original value");

    const auto opts = NativeBookmark::privatePublishOptions();
    check(opts.value(QStringLiteral("pubsub#access_model")).contains(QStringLiteral("whitelist")),
          "PEP bookmarks could be published publicly");
    check(opts.value(QStringLiteral("pubsub#persist_items")).contains(QStringLiteral("true")),
          "PEP bookmarks were not configured for persistence");

    check(!NativeBookmark::fromItem(PubSubItem(QStringLiteral("bad/resource"),
                                               source.documentElement())).isValid(),
          "Invalid room item id was accepted");
    check(!NativeBookmark::fromItem(PubSubItem(QStringLiteral(""),
                                               source.documentElement())).isValid(),
          "Empty room item id was accepted");
    return 0;
}
