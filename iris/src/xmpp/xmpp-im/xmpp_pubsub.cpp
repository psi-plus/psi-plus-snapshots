/*
 * xmpp_pubsub.cpp - XEP-0060/XEP-0163 client-side PubSub helpers
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#include "xmpp_pubsub.h"

#include "xmpp_client.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"

#include <QDomDocument>
#include <QDomElement>
#include <QPointer>

namespace XMPP {
namespace {
    constexpr auto PubSubNs      = "http://jabber.org/protocol/pubsub";
    constexpr auto PubSubOwnerNs = "http://jabber.org/protocol/pubsub#owner";
    constexpr auto XDataNs       = "jabber:x:data";

    QDomElement makePubSub(QDomDocument *doc)
    {
        return doc->createElementNS(QLatin1String(PubSubNs), QStringLiteral("pubsub"));
    }

    void appendDataForm(QDomDocument *doc, QDomElement &parent, const QString &formType, const PubSubOptions &options)
    {
        QDomElement x = doc->createElementNS(QLatin1String(XDataNs), QStringLiteral("x"));
        x.setAttribute(QStringLiteral("type"), QStringLiteral("submit"));

        QDomElement formField = doc->createElementNS(QLatin1String(XDataNs), QStringLiteral("field"));
        formField.setAttribute(QStringLiteral("var"), QStringLiteral("FORM_TYPE"));
        formField.setAttribute(QStringLiteral("type"), QStringLiteral("hidden"));
        formField.appendChild(textTagNS(doc, QLatin1String(XDataNs), QStringLiteral("value"), formType));
        x.appendChild(formField);

        for (auto it = options.cbegin(); it != options.cend(); ++it) {
            QDomElement field = doc->createElementNS(QLatin1String(XDataNs), QStringLiteral("field"));
            field.setAttribute(QStringLiteral("var"), it.key());
            for (const auto &value : it.value())
                field.appendChild(textTagNS(doc, QLatin1String(XDataNs), QStringLiteral("value"), value));
            x.appendChild(field);
        }
        parent.appendChild(x);
    }

    QDomElement directChildNS(const QDomElement &parent, const QString &name, const QString &ns)
    {
        for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            const QString local
                = child.localName().isEmpty() ? child.tagName().section(QLatin1Char(':'), -1) : child.localName();
            if (local == name && child.namespaceURI() == ns)
                return child;
        }
        return {};
    }

} // namespace

class PubSubItemsTask::Private {
public:
    Jid                 service;
    QString             node;
    QStringList         ids;
    int                 maxItems = 0;
    QList<QDomDocument> documents;
    QList<PubSubItem>   items;
};

PubSubItemsTask::PubSubItemsTask(Task *parent) : Task(parent), d(std::make_unique<Private>()) { }
PubSubItemsTask::~PubSubItemsTask() = default;

void PubSubItemsTask::get(const Jid &service, const QString &node, const QStringList &itemIds, int maxItems)
{
    d->service  = service;
    d->node     = node;
    d->ids      = itemIds;
    d->maxItems = maxItems;
    d->documents.clear();
    d->items.clear();
}

const QList<PubSubItem> &PubSubItemsTask::items() const { return d->items; }

void PubSubItemsTask::onGo()
{
    auto iq     = createIQ(doc(), QStringLiteral("get"), d->service.full(), id());
    auto pubsub = makePubSub(doc());
    auto items  = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("items"));
    items.setAttribute(QStringLiteral("node"), d->node);
    if (d->maxItems > 0)
        items.setAttribute(QStringLiteral("max_items"), d->maxItems);
    for (const auto &itemId : d->ids) {
        auto item = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("item"));
        item.setAttribute(QStringLiteral("id"), itemId);
        items.appendChild(item);
    }
    pubsub.appendChild(items);
    iq.appendChild(pubsub);
    send(iq);
}

bool PubSubItemsTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, d->service, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) != QLatin1String("result")) {
        setError(stanza);
        return true;
    }

    const auto pubsub = directChildNS(stanza, QStringLiteral("pubsub"), QLatin1String(PubSubNs));
    const auto items  = directChildNS(pubsub, QStringLiteral("items"), QLatin1String(PubSubNs));
    for (auto item = items.firstChildElement(); !item.isNull(); item = item.nextSiblingElement()) {
        const QString local
            = item.localName().isEmpty() ? item.tagName().section(QLatin1Char(':'), -1) : item.localName();
        if (local != QLatin1String("item"))
            continue;
        QDomDocument owned;
        auto         payload = item.firstChildElement();
        if (!payload.isNull())
            owned.appendChild(owned.importNode(payload, true));
        d->documents.append(owned);
        d->items.append(PubSubItem(item.attribute(QStringLiteral("id")), d->documents.constLast().documentElement()));
    }
    setSuccess();
    return true;
}

class PubSubPublishTask::Private {
public:
    Jid           service;
    QString       node;
    QString       itemId;
    QDomDocument  payloadDoc;
    PubSubOptions options;
    QString       publishedId;
};

PubSubPublishTask::PubSubPublishTask(Task *parent) : Task(parent), d(std::make_unique<Private>()) { }
PubSubPublishTask::~PubSubPublishTask() = default;

void PubSubPublishTask::publish(const Jid &service, const QString &node, const PubSubItem &item,
                                const PubSubOptions &publishOptions)
{
    d->service = service;
    d->node    = node;
    d->itemId  = item.id();
    d->options = publishOptions;
    d->publishedId.clear();
    d->payloadDoc.clear();
    if (!item.payload().isNull())
        d->payloadDoc.appendChild(d->payloadDoc.importNode(item.payload(), true));
}

QString PubSubPublishTask::publishedId() const { return d->publishedId; }

void PubSubPublishTask::onGo()
{
    auto iq      = createIQ(doc(), QStringLiteral("set"), d->service.full(), id());
    auto pubsub  = makePubSub(doc());
    auto publish = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("publish"));
    publish.setAttribute(QStringLiteral("node"), d->node);
    auto item = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("item"));
    if (!d->itemId.isEmpty())
        item.setAttribute(QStringLiteral("id"), d->itemId);
    if (!d->payloadDoc.documentElement().isNull())
        item.appendChild(doc()->importNode(d->payloadDoc.documentElement(), true));
    publish.appendChild(item);
    pubsub.appendChild(publish);

    if (!d->options.isEmpty()) {
        auto publishOptions = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("publish-options"));
        appendDataForm(doc(), publishOptions, QStringLiteral("http://jabber.org/protocol/pubsub#publish-options"),
                       d->options);
        pubsub.appendChild(publishOptions);
    }
    iq.appendChild(pubsub);
    send(iq);
}

bool PubSubPublishTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, d->service, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) != QLatin1String("result")) {
        setError(stanza);
        return true;
    }
    const auto pubsub  = directChildNS(stanza, QStringLiteral("pubsub"), QLatin1String(PubSubNs));
    const auto publish = directChildNS(pubsub, QStringLiteral("publish"), QLatin1String(PubSubNs));
    const auto item    = publish.firstChildElement(QStringLiteral("item"));
    d->publishedId     = item.isNull() ? d->itemId : item.attribute(QStringLiteral("id"));
    setSuccess();
    return true;
}

class PubSubCreateTask::Private {
public:
    Jid           service;
    QString       node;
    PubSubOptions options;
};

PubSubCreateTask::PubSubCreateTask(Task *parent) : Task(parent), d(std::make_unique<Private>()) { }
PubSubCreateTask::~PubSubCreateTask() = default;

void PubSubCreateTask::create(const Jid &service, const QString &node, const PubSubOptions &nodeOptions)
{
    d->service = service;
    d->node    = node;
    d->options = nodeOptions;
}

void PubSubCreateTask::onGo()
{
    auto iq     = createIQ(doc(), QStringLiteral("set"), d->service.full(), id());
    auto pubsub = makePubSub(doc());
    auto create = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("create"));
    create.setAttribute(QStringLiteral("node"), d->node);
    pubsub.appendChild(create);
    if (!d->options.isEmpty()) {
        auto configure = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("configure"));
        appendDataForm(doc(), configure, QStringLiteral("http://jabber.org/protocol/pubsub#node_config"), d->options);
        pubsub.appendChild(configure);
    }
    iq.appendChild(pubsub);
    send(iq);
}

bool PubSubCreateTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, d->service, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) == QLatin1String("result"))
        setSuccess();
    else
        setError(stanza);
    return true;
}

class PubSubConfigureTask::Private {
public:
    Jid           service;
    QString       node;
    PubSubOptions options;
};

PubSubConfigureTask::PubSubConfigureTask(Task *parent) : Task(parent), d(std::make_unique<Private>()) { }
PubSubConfigureTask::~PubSubConfigureTask() = default;

void PubSubConfigureTask::configure(const Jid &service, const QString &node, const PubSubOptions &nodeOptions)
{
    d->service = service;
    d->node    = node;
    d->options = nodeOptions;
}

void PubSubConfigureTask::onGo()
{
    auto iq        = createIQ(doc(), QStringLiteral("set"), d->service.full(), id());
    auto pubsub    = doc()->createElementNS(QLatin1String(PubSubOwnerNs), QStringLiteral("pubsub"));
    auto configure = doc()->createElementNS(QLatin1String(PubSubOwnerNs), QStringLiteral("configure"));
    configure.setAttribute(QStringLiteral("node"), d->node);
    appendDataForm(doc(), configure, QStringLiteral("http://jabber.org/protocol/pubsub#node_config"), d->options);
    pubsub.appendChild(configure);
    iq.appendChild(pubsub);
    send(iq);
}

bool PubSubConfigureTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, d->service, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) == QLatin1String("result"))
        setSuccess();
    else
        setError(stanza);
    return true;
}

class PubSubRetractTask::Private {
public:
    Jid     service;
    QString node;
    QString itemId;
    bool    notify = true;
};

PubSubRetractTask::PubSubRetractTask(Task *parent) : Task(parent), d(std::make_unique<Private>()) { }
PubSubRetractTask::~PubSubRetractTask() = default;

void PubSubRetractTask::retract(const Jid &service, const QString &node, const QString &itemId, bool notify)
{
    d->service = service;
    d->node    = node;
    d->itemId  = itemId;
    d->notify  = notify;
}

void PubSubRetractTask::onGo()
{
    auto iq      = createIQ(doc(), QStringLiteral("set"), d->service.full(), id());
    auto pubsub  = makePubSub(doc());
    auto retract = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("retract"));
    retract.setAttribute(QStringLiteral("node"), d->node);
    retract.setAttribute(QStringLiteral("notify"), d->notify ? QStringLiteral("true") : QStringLiteral("false"));
    auto item = doc()->createElementNS(QLatin1String(PubSubNs), QStringLiteral("item"));
    item.setAttribute(QStringLiteral("id"), d->itemId);
    retract.appendChild(item);
    pubsub.appendChild(retract);
    iq.appendChild(pubsub);
    send(iq);
}

bool PubSubRetractTask::take(const QDomElement &stanza)
{
    if (!iqVerify(stanza, d->service, id()))
        return false;
    if (stanza.attribute(QStringLiteral("type")) == QLatin1String("result"))
        setSuccess();
    else
        setError(stanza);
    return true;
}

class PubSubManager::Private {
public:
    class Subscriber final : public JT_PushMessage::Subscriber {
    public:
        explicit Subscriber(PubSubManager *manager) : manager_(manager) { }

        bool messageEvent(Message &message, int userData, bool nested) override
        {
            Q_UNUSED(userData)
            // Forwarded/archive messages still expose pubSubEvents() to their
            // consumer, but must not be replayed as live PubSub notifications.
            if (nested)
                return false;

            const Jid service = message.from();
            for (const auto &event : message.pubSubEvents()) {
                emit manager_->eventReceived(service, event);
                switch (event.type()) {
                case PubSubEvent::Type::Items:
                    for (const auto &item : event.items())
                        emit manager_->itemPublished(service, event.node(), item);
                    for (const auto &retraction : event.retractions())
                        emit manager_->itemRetracted(service, event.node(), retraction.id());
                    break;
                case PubSubEvent::Type::Purge:
                    emit manager_->nodePurged(service, event.node());
                    break;
                case PubSubEvent::Type::Delete:
                    emit manager_->nodeDeleted(service, event.node());
                    break;
                case PubSubEvent::Type::Collection:
                case PubSubEvent::Type::Configuration:
                case PubSubEvent::Type::Subscription:
                case PubSubEvent::Type::Unknown:
                    break;
                }
            }
            // Observing PubSub must never consume an ordinary Message.
            return false;
        }

    private:
        PubSubManager *manager_;
    };

    explicit Private(PubSubManager *manager) : subscriber(std::make_unique<Subscriber>(manager)) { }

    Client                     *client = nullptr;
    QPointer<JT_PushMessage>    pushMessage;
    std::unique_ptr<Subscriber> subscriber;
};

PubSubManager::PubSubManager(Client *client) : QObject(client), d(std::make_unique<Private>(this))
{
    d->client = client;
}

PubSubManager::~PubSubManager()
{
    if (d->pushMessage)
        d->pushMessage->unsubscribeMessage(d->subscriber.get());
}

void PubSubManager::setPushMessage(JT_PushMessage *pushMessage)
{
    if (d->pushMessage == pushMessage)
        return;
    if (d->pushMessage)
        d->pushMessage->unsubscribeMessage(d->subscriber.get());
    d->pushMessage = pushMessage;
    if (d->pushMessage)
        d->pushMessage->subscribeMessage(d->subscriber.get(), 0);
}

PubSubItemsTask *PubSubManager::items(const Jid &service, const QString &node, const QStringList &itemIds, int maxItems)
{
    auto task = new PubSubItemsTask(d->client->rootTask());
    task->get(service, node, itemIds, maxItems);
    return task;
}

PubSubPublishTask *PubSubManager::publish(const Jid &service, const QString &node, const PubSubItem &item,
                                          const PubSubOptions &publishOptions)
{
    auto task = new PubSubPublishTask(d->client->rootTask());
    task->publish(service, node, item, publishOptions);
    return task;
}

PubSubCreateTask *PubSubManager::createNode(const Jid &service, const QString &node, const PubSubOptions &nodeOptions)
{
    auto task = new PubSubCreateTask(d->client->rootTask());
    task->create(service, node, nodeOptions);
    return task;
}

PubSubConfigureTask *PubSubManager::configureNode(const Jid &service, const QString &node,
                                                  const PubSubOptions &nodeOptions)
{
    auto task = new PubSubConfigureTask(d->client->rootTask());
    task->configure(service, node, nodeOptions);
    return task;
}

PubSubRetractTask *PubSubManager::retract(const Jid &service, const QString &node, const QString &itemId, bool notify)
{
    auto task = new PubSubRetractTask(d->client->rootTask());
    task->retract(service, node, itemId, notify);
    return task;
}

} // namespace XMPP
