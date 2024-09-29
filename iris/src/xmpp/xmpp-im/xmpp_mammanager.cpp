/*
 * xmpp_mammanager.cpp - XEP-0313 Message Archive Management
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

#include "xmpp_mammanager.h"

using namespace XMPP;

class MAMManager::Private {
public:
    int     mamPageSize;
    int     mamMaxMessages;
    bool    flipPages;
    bool    backwards;
    Client *client;
};

MAMManager::MAMManager(Client *client, int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d = new Private;

    d->client         = client;
    d->mamPageSize    = mamPageSize;
    d->mamMaxMessages = mamMaxMessages;
    d->flipPages      = flipPages;
    d->backwards      = backwards;
}

MAMManager::~MAMManager() { delete d; }

// TODO: review the safety of these methods/object lifetimes
MAMTask *MAMManager::getFullArchive(const Jid &j, const bool allowMUCArchives)
{
    auto task = new MAMTask(d->client->rootTask());

    task->get(j, QString(), QString(), allowMUCArchives, d->mamPageSize, d->mamMaxMessages, d->flipPages, d->backwards);
    return task;
}

MAMTask *MAMManager::getArchiveByIDRange(const Jid &j, const QString &fromID, const QString &toID,
                                         const bool allowMUCArchives)
{
    auto task = new MAMTask(d->client->rootTask());

    task->get(j, fromID, toID, allowMUCArchives, d->mamPageSize, d->mamMaxMessages, d->flipPages, d->backwards);
    return task;
}

MAMTask *MAMManager::getArchiveByTimeRange(const Jid &j, const QDateTime &from, const QDateTime &to,
                                           const bool allowMUCArchives)
{
    auto task = new MAMTask(d->client->rootTask());

    task->get(j, from, to, allowMUCArchives, d->mamPageSize, d->mamMaxMessages, d->flipPages, d->backwards);
    return task;
}

MAMTask *MAMManager::getLatestMessagesFromArchive(const Jid &j, const QString &fromID, const bool allowMUCArchives,
                                                  int amount)
{
    auto task = new MAMTask(d->client->rootTask());

    task->get(j, fromID, QString(), allowMUCArchives, d->mamPageSize, amount, true, true);
    return task;
}

MAMTask *MAMManager::getMessagesBeforeID(const Jid &j, const QString &toID, const bool allowMUCArchives, int amount)
{
    auto task = new MAMTask(d->client->rootTask());

    task->get(j, QString(), toID, allowMUCArchives, d->mamPageSize, amount, true, true);
    return task;
}
