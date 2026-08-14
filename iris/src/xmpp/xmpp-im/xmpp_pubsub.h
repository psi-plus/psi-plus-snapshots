/*
 * xmpp_pubsub.h - XEP-0060/XEP-0163 client-side PubSub helpers
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XMPP_PUBSUB_H
#define XMPP_PUBSUB_H

#include "xmpp/jid/jid.h"
#include "xmpp_pubsubevent.h"
#include "xmpp_pubsubitem.h"
#include "xmpp_task.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

#include <memory>

namespace XMPP {

class JT_PushMessage;

using PubSubOptions = QMap<QString, QStringList>;

class PubSubItemsTask : public Task {
    Q_OBJECT
public:
    explicit PubSubItemsTask(Task *parent);
    ~PubSubItemsTask() override;

    void get(const Jid &service, const QString &node, const QStringList &itemIds = {}, int maxItems = 0);
    const QList<PubSubItem> &items() const;

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};

class PubSubPublishTask : public Task {
    Q_OBJECT
public:
    explicit PubSubPublishTask(Task *parent);
    ~PubSubPublishTask() override;

    void    publish(const Jid &service, const QString &node, const PubSubItem &item,
                    const PubSubOptions &publishOptions = {});
    QString publishedId() const;

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};

class PubSubCreateTask : public Task {
    Q_OBJECT
public:
    explicit PubSubCreateTask(Task *parent);
    ~PubSubCreateTask() override;

    void create(const Jid &service, const QString &node, const PubSubOptions &nodeOptions = {});

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};

class PubSubConfigureTask : public Task {
    Q_OBJECT
public:
    explicit PubSubConfigureTask(Task *parent);
    ~PubSubConfigureTask() override;

    void configure(const Jid &service, const QString &node, const PubSubOptions &nodeOptions);

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};

class PubSubRetractTask : public Task {
    Q_OBJECT
public:
    explicit PubSubRetractTask(Task *parent);
    ~PubSubRetractTask() override;

    void retract(const Jid &service, const QString &node, const QString &itemId, bool notify = true);

protected:
    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};

/** Generic PubSub/PEP facade shared by OMEMO and application protocols. */
class PubSubManager : public QObject {
    Q_OBJECT
public:
    explicit PubSubManager(Client *client);
    ~PubSubManager() override;

    PubSubItemsTask *items(const Jid &service, const QString &node, const QStringList &itemIds = {}, int maxItems = 0);
    PubSubPublishTask   *publish(const Jid &service, const QString &node, const PubSubItem &item,
                                 const PubSubOptions &publishOptions = {});
    PubSubCreateTask    *createNode(const Jid &service, const QString &node, const PubSubOptions &nodeOptions = {});
    PubSubConfigureTask *configureNode(const Jid &service, const QString &node, const PubSubOptions &nodeOptions);
    PubSubRetractTask   *retract(const Jid &service, const QString &node, const QString &itemId, bool notify = true);

signals:
    void eventReceived(const Jid &service, const PubSubEvent &event);
    void itemPublished(const Jid &service, const QString &node, const PubSubItem &item);
    void itemRetracted(const Jid &service, const QString &node, const QString &itemId);
    void nodePurged(const Jid &service, const QString &node);
    void nodeDeleted(const Jid &service, const QString &node);

private:
    friend class Client;
    void setPushMessage(JT_PushMessage *pushMessage);

    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

#endif // XMPP_PUBSUB_H
