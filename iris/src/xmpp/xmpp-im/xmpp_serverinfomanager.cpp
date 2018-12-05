/*
 * xmpp_serverinfomanager.cpp
 * Copyright (C) 2006  Remko Troncon
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "xmpp_serverinfomanager.h"
#include "xmpp_tasks.h"
#include "xmpp_caps.h"

namespace XMPP {

ServerInfoManager::ServerInfoManager(Client* client)
    : _client(client)
    , _canMessageCarbons(false)
{
    deinitialize();
    // NOTE we can use this class for any server, but for this we shouldn't use roster signal here
    connect(_client, SIGNAL(rosterRequestFinished(bool, int, const QString &)), SLOT(initialize()), Qt::QueuedConnection);
}

void ServerInfoManager::reset()
{
    _hasPEP = false;
    _multicastService.clear();
    _extraServerInfo.clear();
    disconnect(CapsRegistry::instance());
    disconnect(_client, SIGNAL(disconnected()), this, SLOT(deinitialize()));
}

void ServerInfoManager::initialize()
{
    connect(_client, SIGNAL(disconnected()), SLOT(deinitialize()));
    JT_DiscoInfo *jt = new JT_DiscoInfo(_client->rootTask());
    connect(jt, SIGNAL(finished()), SLOT(disco_finished()));
    jt->get(_client->jid().domain());
    jt->go(true);
}

void ServerInfoManager::deinitialize()
{
    reset();
    emit featuresChanged();
}

const QString& ServerInfoManager::multicastService() const
{
    return _multicastService;
}

bool ServerInfoManager::hasPEP() const
{
    return _hasPEP;
}

bool ServerInfoManager::canMessageCarbons() const
{
    return _canMessageCarbons;
}

void ServerInfoManager::disco_finished()
{
    JT_DiscoInfo *jt = static_cast<JT_DiscoInfo *>(sender());
    if (jt->success()) {
        _features = jt->item().features();

        if (_features.hasMulticast())
            _multicastService = _client->jid().domain();

        _canMessageCarbons = _features.hasMessageCarbons();

        // Identities
        DiscoItem::Identities is = jt->item().identities();
        foreach(DiscoItem::Identity i, is) {
            if (i.category == "pubsub" && i.type == "pep")
                _hasPEP = true;
        }

        for (const auto &x: jt->item().extensions()) {
            if (x.type() == XData::Data_Result && x.registrarType() == QLatin1String("http://jabber.org/network/serverinfo")) {
                for (const auto &f: x.fields()) {
                    if (f.type() == XData::Field::Field_ListMulti) {
                        QStringList values;
                        _extraServerInfo.insert(f.var(), f.value()); // covers XEP-0157
                    }
                }
            }
        }

        emit featuresChanged();
    }
}

} // namespace XMPP
