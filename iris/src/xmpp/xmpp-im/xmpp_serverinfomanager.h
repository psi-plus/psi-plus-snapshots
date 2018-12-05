/*
 * xmpp_serverinfomanager.h
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

#ifndef SERVERINFOMANAGER_H
#define SERVERINFOMANAGER_H

#include "xmpp_caps.h"

#include <QObject>
#include <QString>

namespace XMPP {

class Client;
class Features;
class Jid;
class DiscoItem;

class ServerInfoManager : public QObject
{
    Q_OBJECT

public:
    ServerInfoManager(XMPP::Client* client);

    const QString& multicastService() const;
    bool hasPEP() const;
    inline const Features &features() const { return _features; }
    bool canMessageCarbons() const;
    inline const QMap<QString,QStringList> &extraServerInfo() const { return _extraServerInfo; }

signals:
    void featuresChanged();

private slots:
    void disco_finished();
    void initialize();
    void deinitialize();
    void reset();

private:
    XMPP::Client* _client = nullptr;
    CapsSpec _caps;
    Features _features;
    QString _multicastService;
    QMap<QString,QStringList> _extraServerInfo; // XEP-0128, XEP-0157
    bool _featuresRequested;
    bool _hasPEP;
    bool _canMessageCarbons;
};

} // namespace XMPP

#endif
