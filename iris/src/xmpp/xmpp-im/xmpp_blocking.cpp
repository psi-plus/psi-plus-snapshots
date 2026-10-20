/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "xmpp_blocking.h"
#include "xmpp_client.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {
namespace {
constexpr auto BlockingNs = "urn:xmpp:blocking";

QList<Jid> parseItems(const QDomElement &parent, bool *valid)
{
    QList<Jid> jids;
    *valid = true;
    for (auto item = parent.firstChildElement(); !item.isNull(); item = item.nextSiblingElement()) {
        if (item.namespaceURI() != QLatin1String(BlockingNs) || item.tagName() != QLatin1String("item")) {
            *valid = false;
            return {};
        }
        const Jid jid(item.attribute(QStringLiteral("jid")));
        if (!jid.isValid() || jid.isEmpty()) {
            *valid = false;
            return {};
        }
        if (!jids.contains(jid))
            jids.append(jid);
    }
    return jids;
}
} // namespace

BlockingTask::BlockingTask(Task *parent) : Task(parent) { }

void BlockingTask::get()
{
    operation_ = QStringLiteral("blocklist");
    requestedJids_.clear();
    blockedJids_.clear();
}
void BlockingTask::block(const QList<Jid> &jids)
{
    operation_ = QStringLiteral("block");
    requestedJids_ = jids;
}
void BlockingTask::unblock(const QList<Jid> &jids)
{
    operation_ = QStringLiteral("unblock");
    requestedJids_ = jids;
}

void BlockingTask::onGo()
{
    if (operation_.isEmpty() || (operation_ == QLatin1String("block") && requestedJids_.isEmpty())) {
        setError();
        return;
    }
    const bool query = operation_ == QLatin1String("blocklist");
    auto iq = createIQ(doc(), query ? QStringLiteral("get") : QStringLiteral("set"), QString(), id());
    auto command = doc()->createElementNS(QLatin1String(BlockingNs), operation_);
    for (const auto &jid : requestedJids_) {
        if (!jid.isValid() || jid.isEmpty()) {
            setError();
            return;
        }
        auto item = doc()->createElementNS(QLatin1String(BlockingNs), QStringLiteral("item"));
        item.setAttribute(QStringLiteral("jid"), jid.full());
        command.appendChild(item);
    }
    iq.appendChild(command);
    send(iq);
}

bool BlockingTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, Jid(), id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) != QLatin1String("result")) {
        setError(stanza);
        return true;
    }
    if (operation_ == QLatin1String("blocklist")) {
        const auto list = stanza.firstChildElement(QStringLiteral("blocklist"));
        if (list.isNull() || list.namespaceURI() != QLatin1String(BlockingNs)) {
            setError();
            return true;
        }
        bool valid = false;
        blockedJids_ = parseItems(list, &valid);
        if (!valid) {
            setError();
            return true;
        }
    }
    setSuccess();
    return true;
}

BlockingPushTask::BlockingPushTask(Task *parent) : Task(parent) { }

bool BlockingPushTask::take(const QDomElement &stanza)
{
    if (stanza.tagName() != QLatin1String("iq")
        || stanza.attribute(QStringLiteral("type")) != QLatin1String("set"))
        return false;
    const auto command = stanza.firstChildElement();
    if (command.isNull() || command.namespaceURI() != QLatin1String(BlockingNs))
        return false;
    const QString operation = command.tagName();
    if (operation != QLatin1String("block") && operation != QLatin1String("unblock"))
        return false;
    // XEP-0191 pushes are sent by our own server; never accept a push from
    // another entity, even one supplying syntactically valid blocked items.
    const Jid from(stanza.attribute(QStringLiteral("from")));
    if (!from.isEmpty() && from.full() != client()->jid().domain()
        && !(from.resource().isEmpty() && from.bare() == client()->jid().bare()))
        return false;

    bool valid = false;
    const auto jids = parseItems(command, &valid);
    const bool blocking = operation == QLatin1String("block");
    if (!valid || (blocking && jids.isEmpty()))
        return false;

    auto response = createIQ(doc(), QStringLiteral("result"), stanza.attribute(QStringLiteral("from")),
                             stanza.attribute(QStringLiteral("id")));
    send(response);
    emit pushed(jids, blocking, !blocking && jids.isEmpty());
    return true;
}
} // namespace XMPP
