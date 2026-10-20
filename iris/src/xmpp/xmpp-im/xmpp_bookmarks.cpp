/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "xmpp_bookmarks.h"

namespace XMPP {
namespace {
const QString ns = QStringLiteral("urn:xmpp:bookmarks:1");

QString childText(const QDomElement &parent, const QString &name)
{
    const auto child = parent.firstChildElement(name);
    return child.namespaceURI() == ns ? child.text() : QString();
}
} // namespace

QString NativeBookmark::node() { return ns; }

PubSubOptions NativeBookmark::privatePublishOptions()
{
    return { { QStringLiteral("pubsub#persist_items"), { QStringLiteral("true") } },
             { QStringLiteral("pubsub#max_items"), { QStringLiteral("max") } },
             { QStringLiteral("pubsub#send_last_published_item"), { QStringLiteral("never") } },
             { QStringLiteral("pubsub#access_model"), { QStringLiteral("whitelist") } } };
}

NativeBookmark NativeBookmark::fromItem(const PubSubItem &item)
{
    NativeBookmark result;
    const Jid room(item.id());
    const auto payload = item.payload();
    if (!room.isValid() || room.isEmpty() || !room.resource().isEmpty()
        || payload.namespaceURI() != ns || payload.tagName() != QLatin1String("conference"))
        return result;

    result.room_ = room;
    result.name_ = payload.attribute(QStringLiteral("name"));
    const QString join = payload.attribute(QStringLiteral("autojoin")); 
    result.autoJoin_ = join == QLatin1String("true") || join == QLatin1String("1");
    result.nick_ = childText(payload, QStringLiteral("nick"));
    result.password_ = childText(payload, QStringLiteral("password"));

    // Unknown client data may live under <extensions/>. Preserve only that
    // subtree; known bookmark fields remain independent of the source DOM.
    const auto extensions = payload.firstChildElement(QStringLiteral("extensions"));
    if (!extensions.isNull() && extensions.namespaceURI() == ns)
        result.extensions_.appendChild(result.extensions_.importNode(extensions, true));
    return result;
}

NativeBookmark NativeBookmark::create(const Jid &room, const QString &name, bool autoJoin,
                                      const QString &nick, const QString &password)
{
    NativeBookmark result;
    if (room.isValid() && !room.isEmpty() && room.resource().isEmpty()) {
        result.room_ = room;
        result.name_ = name;
        result.autoJoin_ = autoJoin;
        result.nick_ = nick;
        result.password_ = password;
    }
    return result;
}

bool NativeBookmark::isValid() const
{
    return room_.isValid() && !room_.isEmpty() && room_.resource().isEmpty();
}
Jid NativeBookmark::room() const { return room_; }
QString NativeBookmark::name() const { return name_; }
QString NativeBookmark::nick() const { return nick_; }
QString NativeBookmark::password() const { return password_; }
bool NativeBookmark::autoJoin() const { return autoJoin_; }
void NativeBookmark::setName(const QString &name) { name_ = name; }
void NativeBookmark::setAutoJoin(bool enabled) { autoJoin_ = enabled; }
void NativeBookmark::setNick(const QString &nick) { nick_ = nick; }
void NativeBookmark::setPassword(const QString &password) { password_ = password; }

PubSubItem NativeBookmark::toItem(QDomDocument &document) const
{
    if (!isValid())
        return {};

    auto conference = document.createElementNS(ns, QStringLiteral("conference"));
    conference.setAttribute(QStringLiteral("name"), name_);
    conference.setAttribute(QStringLiteral("autojoin"), autoJoin_ ? QStringLiteral("true") : QStringLiteral("false"));

    const auto appendText = [&document, &conference](const QString &tag, const QString &value) {
        if (value.isEmpty())
            return;
        auto element = document.createElementNS(ns, tag);
        element.appendChild(document.createTextNode(value));
        conference.appendChild(element);
    };
    appendText(QStringLiteral("nick"), nick_);
    appendText(QStringLiteral("password"), password_);

    if (!extensions_.documentElement().isNull())
        conference.appendChild(document.importNode(extensions_.documentElement(), true));
    return PubSubItem(room_.bare(), conference);
}
} // namespace XMPP
