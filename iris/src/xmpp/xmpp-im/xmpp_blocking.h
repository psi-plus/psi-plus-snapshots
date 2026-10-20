/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef XMPP_BLOCKING_H
#define XMPP_BLOCKING_H

#include <iris/xmpp-im/xmpp_task.h>
#include <iris/xmpp_jid.h>
#include <QStringList>

namespace XMPP {

// XEP-0191. The initial blocklist is fetched before the client modifies it.
class IRIS_EXPORT BlockingTask : public Task {
public:
    explicit BlockingTask(Task *parent);
    void get();
    void block(const QList<Jid> &jids);
    void unblock(const QList<Jid> &jids); // Empty list unblocks everyone.
    const QList<Jid> &blockedJids() const { return blockedJids_; }

    void onGo() override;
    bool take(const QDomElement &stanza) override;

private:
    QString operation_;
    QList<Jid> requestedJids_;
    QList<Jid> blockedJids_;
};

// Persistent push receiver. It never consumes an unrelated IQ.
class IRIS_EXPORT BlockingPushTask : public Task {
    Q_OBJECT
public:
    explicit BlockingPushTask(Task *parent);
    bool take(const QDomElement &stanza) override;

signals:
    void pushed(const QList<XMPP::Jid> &jids, bool blocked, bool all);

private:
    bool initialized_ = false;
};
} // namespace XMPP
#endif // XMPP_BLOCKING_H
