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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "irisnetplugin.h"

#include <QNetworkInterface>

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
#ifdef Q_OS_LINUX
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

class InterfaceMonitor : public QObject {
    Q_OBJECT
public:
    InterfaceMonitor()
    {
        struct sockaddr_nl sa;

        memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;

        netlinkFd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (netlinkFd == -1) {
            return;
        }
        if (bind(netlinkFd, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
            close(netlinkFd);
            netlinkFd = -1;
            return;
        }
        notifier = new QSocketNotifier(netlinkFd, QSocketNotifier::Read, this);

        connect(notifier, &QSocketNotifier::activated, this,
                [this](QSocketDescriptor, QSocketNotifier::Type) { emit changed(); });
    }

    ~InterfaceMonitor()
    {
        if (notifier) {
            delete notifier;
            close(netlinkFd);
        }
    }

signals:
    void changed();

private:
    QSocketNotifier *notifier  = nullptr;
    int              netlinkFd = -1;
};

#else
// not linux version. polling? TODO. probably with Qt6 we can use QNetworkStatusMonitor
class InterfaceMonitor : public QObject {
    Q_OBJECT
public:
    InterfaceMonitor() { }

signals:
    void changed();
};
#endif
#else // old Qt < 5.15
class InterfaceMonitor : public QObject {
    Q_OBJECT
public:
    InterfaceMonitor()
    {
        connect(&ncm, SIGNAL(configurationAdded(QNetworkConfiguration)), SIGNAL(changed()));
        connect(&ncm, SIGNAL(configurationChanged(QNetworkConfiguration)), SIGNAL(changed()));
        connect(&ncm, SIGNAL(configurationRemoved(QNetworkConfiguration)), SIGNAL(changed()));
    }

signals:
    void changed();

private:
    QNetworkConfigurationManager ncm;
};
#endif

namespace XMPP {
class IrisQtNet : public NetInterfaceProvider {
    Q_OBJECT
    Q_INTERFACES(XMPP::NetInterfaceProvider)
public:
    QList<Info>      info;
    InterfaceMonitor monitor;

    IrisQtNet() { connect(&monitor, &InterfaceMonitor::changed, this, &IrisQtNet::check); }

    void start() { poll(); }

    QList<Info> interfaces() const { return info; }

    void poll()
    {
        QList<Info> ifaces;

        auto const interfaces = QNetworkInterface::allInterfaces();
        for (auto &iface : interfaces) {
            Info i;
            i.id         = iface.name();
            i.name       = iface.humanReadableName();
            i.isLoopback = bool(iface.flags() & QNetworkInterface::IsLoopBack);
            for (auto &ae : iface.addressEntries()) {
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

class IrisQtNetProvider : public IrisNetProvider {
    Q_OBJECT
    Q_INTERFACES(XMPP::IrisNetProvider)
public:
    NetInterfaceProvider *createNetInterfaceProvider() { return new IrisQtNet; }
};

IrisNetProvider *irisnet_createQtNetProvider() { return new IrisQtNetProvider; }
} // namespace XMPP

#include "netinterface_qtnet.moc"
