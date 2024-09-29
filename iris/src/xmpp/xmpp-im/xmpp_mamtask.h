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

#include "xmpp/jid/jid.h"
#include "xmpp_subsets.h"
#include "xmpp_task.h"
#include "xmpp_xdata.h"
#include "xmpp_xmlcommon.h"

#include <QDateTime>

#define XMPP_MAM_NAMESPACE QLatin1String("urn:xmpp:mam:2")

class QDomElement;
class QString;

namespace XMPP {
class Jid;

class MAMTask : public Task {
    Q_OBJECT
public:
    explicit MAMTask(Task *);
    MAMTask(const MAMTask &x);
    ~MAMTask();

    const QList<QDomElement> &archive() const;

    // Time filter
    void get(const Jid &j, const QDateTime &from = QDateTime(), const QDateTime &to = QDateTime(),
             const bool allowMUCArchives = true, int mamPageSize = 10, int mamMaxMessages = 0, bool flipPages = true,
             bool backwards = true);

    // ID Filter
    void get(const Jid &j, const QString &fromID = QString(), const QString &toID = QString(),
             const bool allowMUCArchives = true, int mamPageSize = 10, int mamMaxMessages = 0, bool flipPages = true,
             bool backwards = true);

    void onGo();
    bool take(const QDomElement &);

private:
    class Private;
    Private *d;
};
} // namespace XMPP

#endif
