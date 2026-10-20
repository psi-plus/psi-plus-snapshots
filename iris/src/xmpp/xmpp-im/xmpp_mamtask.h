/*
 * xmpp_mamtask.h - XEP-0313 Message Archive Management
 * Copyright (C) 2024 mcneb10
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_MAM_TASK_H
#define XMPP_MAM_TASK_H

#include <iris/jid/jid.h>
#include <iris/xmpp-im/xmpp_subsets.h>
#include <iris/xmpp-im/xmpp_task.h>
#include <iris/xmpp-im/xmpp_xdata.h>
#include <iris/xmpp-im/xmpp_xmlcommon.h>

#include <QDateTime>

#include <memory>

#define XMPP_MAM_NAMESPACE QLatin1String("urn:xmpp:mam:2")

class QDomElement;
class QString;

namespace XMPP {
class Jid;
class Features;

class MAMTask : public Task {
    Q_OBJECT
public:
    explicit MAMTask(Task *);
    MAMTask(const MAMTask &)            = delete;
    MAMTask &operator=(const MAMTask &) = delete;
    ~MAMTask();

    const QList<QDomElement> &archive() const;

    // By default, every result is retained in archive(). Disable buffering
    // for large catch-up queries and consume resultReceived() incrementally.
    void setCollectResults(bool enabled);

    // The archive being queried (e.g. a MUC room) is distinct from the
    // optional 'with' filter passed to get(). Empty means the own archive.
    void setArchiveJid(const Jid &archiveJid);
    // Pass features advertised by disco#info for this exact archive JID.
    // Without them, only baseline XEP-0313 operations are attempted.
    void setArchiveFeatures(const Features &features);

    // A bounded request can finish successfully without exhausting the archive.
    bool isComplete() const;
    // XEP-0313 'stable=false' means results need later confirmation.
    bool isStable() const;
    // Optional RSM metadata; -1 means unknown. Count describes matching
    // archive events (possibly approximate), not visible chat messages.
    int resultCount() const;
    // Index supplied for the first physical page. A later page cannot fill
    // in missing metadata for the first page's results.
    int firstResultIndex() const;

    // Configuration is only valid before go(); Task is one-shot.
    // Time filter
    void get(const Jid &j, const QDateTime &from = QDateTime(), const QDateTime &to = QDateTime(),
             const bool allowMUCArchives = true, int mamPageSize = 10, int mamMaxMessages = 0, bool flipPages = true,
             bool backwards = true);

    // ID Filter
    void get(const Jid &j, const QString &fromID = QString(), const QString &toID = QString(),
             const bool allowMUCArchives = true, int mamPageSize = 10, int mamMaxMessages = 0, bool flipPages = true,
             bool backwards = true);

    // Indexed random-access page (XEP-0059); servers may reject it.
    // Returns one bounded page without moving the ordinary cursor.
    void getAtIndex(const Jid &with, int index, int pageSize, bool allowMUCArchives = true);

    void onGo() override;
    bool take(const QDomElement &) override;

signals:
    // Emitted for each validated result, before the final fin IQ.
    void resultReceived(const QDomElement &result);

private:
    // Creates the next child page Task; never examines stanza or IQ content.
    void startPage();

    class Private;
    std::unique_ptr<Private> d;
};
} // namespace XMPP

#endif
