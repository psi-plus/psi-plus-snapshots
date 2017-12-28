/*
 * Copyright (C) 2017  Sergey Ilinykh
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "irisnetplugin.h"

#include <QNetworkInterface>

namespace XMPP {

class IrisQtNet : public NetInterfaceProvider
{
    Q_OBJECT
    Q_INTERFACES(XMPP::NetInterfaceProvider)
public:
    QList<Info> info;
    QNetworkConfigurationManager ncm;

    IrisQtNet()
    {
        connect(&ncm, SIGNAL(configurationAdded(QNetworkConfiguration)), SLOT(check()));
        connect(&ncm, SIGNAL(configurationChanged(QNetworkConfiguration)), SLOT(check()));
        connect(&ncm, SIGNAL(configurationRemoved(QNetworkConfiguration)), SLOT(check()));
    }

    void start()
    {
        poll();
    }

    QList<Info> interfaces() const
    {
        return info;
    }

    void poll()
    {
        QList<Info> ifaces;

        for (auto &iface: QNetworkInterface::allInterfaces()) {
            Info i;
            i.id = iface.name();
            i.name = iface.humanReadableName();
            i.isLoopback = (iface.flags() & QNetworkInterface::IsLoopBack);
            for (auto &ae: iface.addressEntries()) {
                i.addresses.append(ae.ip());
            }
            ifaces << i;
        }

        info = ifaces;
    }

public slots:
    void check()
    {
        poll();
        emit updated();
    }
};

class IrisQtNetProvider : public IrisNetProvider
{
    Q_OBJECT
    Q_INTERFACES(XMPP::IrisNetProvider)
public:
    NetInterfaceProvider *createNetInterfaceProvider()
    {
        return new IrisQtNet;
    }
};

IrisNetProvider *irisnet_createQtNetProvider()
{
    return new IrisQtNetProvider;
}

}

#include "netinterface_qtnet.moc"
