/*
 * Copyright (C) 2006  Justin Karneges
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

// this code assumes the following ioctls work:
//   SIOCGIFCONF  - get list of devices
//   SIOCGIFFLAGS - get flags about a device

// gateway detection currently only works on linux

#include "irisnetplugin.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>

// for solaris
#ifndef SIOCGIFCONF
# include<sys/sockio.h>
#endif

#ifdef Q_OS_LINUX
static QStringList read_proc_as_lines(const char *procfile)
{
    QStringList out;

    FILE *f = fopen(procfile, "r");
    if(!f)
        return out;

    QByteArray buf;
    while(!feof(f))
    {
        // max read on a proc is 4K
        QByteArray block(4096, 0);
        int ret = fread(block.data(), 1, block.size(), f);
        if(ret <= 0)
            break;
        block.resize(ret);
        buf += block;
    }
    fclose(f);

    QString str = QString::fromLocal8Bit(buf);
    out = str.split('\n', QString::SkipEmptyParts);
    return out;
}

static QHostAddress linux_ipv6_to_qaddr(const QString &in)
{
    QHostAddress out;
    if(in.length() != 32)
        return out;
    quint8 raw[16];
    for(int n = 0; n < 16; ++n)
    {
        bool ok;
        int x = in.mid(n * 2, 2).toInt(&ok, 16);
        if(!ok)
            return out;
        raw[n] = (quint8)x;
    }
    out.setAddress(raw);
    return out;
}

static QHostAddress linux_ipv4_to_qaddr(const QString &in)
{
    QHostAddress out;
    if(in.length() != 8)
        return out;
    quint32 raw;
    unsigned char *rawp = (unsigned char *)&raw;
    for(int n = 0; n < 4; ++n)
    {
        bool ok;
        int x = in.mid(n * 2, 2).toInt(&ok, 16);
        if(!ok)
            return out;
        rawp[n] = (unsigned char )x;
    }
    out.setAddress(raw);
    return out;
}

static QList<XMPP::NetGatewayProvider::Info> get_linux_gateways()
{
    QList<XMPP::NetGatewayProvider::Info> out;

    QStringList lines = read_proc_as_lines("/proc/net/route");
    // skip the first line, so we start at 1
    for(int n = 1; n < lines.count(); ++n)
    {
        const QString &line = lines[n];
        QStringList parts = line.simplified().split(' ', QString::SkipEmptyParts);
        if(parts.count() < 10) // net-tools does 10, but why not 11?
            continue;

        QHostAddress addr = linux_ipv4_to_qaddr(parts[2]);
        if(addr.isNull())
            continue;

        int iflags = parts[3].toInt(0, 16);
        if(!(iflags & RTF_UP))
            continue;

        if(!(iflags & RTF_GATEWAY))
            continue;

        XMPP::NetGatewayProvider::Info g;
        g.ifaceId = parts[0];
        g.gateway = addr;
        out += g;
    }

    lines = read_proc_as_lines("/proc/net/ipv6_route");
    for(int n = 0; n < lines.count(); ++n)
    {
        const QString &line = lines[n];
        QStringList parts = line.simplified().split(' ', QString::SkipEmptyParts);
        if(parts.count() < 10)
            continue;

        QHostAddress addr = linux_ipv6_to_qaddr(parts[4]);
        if(addr.isNull())
            continue;

        int iflags = parts[8].toInt(0, 16);
        if(!(iflags & RTF_UP))
            continue;

        if(!(iflags & RTF_GATEWAY))
            continue;

        XMPP::NetGatewayProvider::Info g;
        g.ifaceId = parts[9];
        g.gateway = addr;
        out += g;
    }

    return out;
}
#endif

static QList<XMPP::NetGatewayProvider::Info> get_unix_gateways()
{
    // support other platforms here
    QList<XMPP::NetGatewayProvider::Info> out;
#ifdef Q_OS_LINUX
    out = get_linux_gateways();
#endif
    return out;
}

namespace XMPP {

class UnixGateway : public NetGatewayProvider
{
    Q_OBJECT
    Q_INTERFACES(XMPP::NetGatewayProvider)
public:
    QList<Info> info;
    //QTimer t;

    UnixGateway() //: t(this)
    {
        //connect(&t, SIGNAL(timeout()), SLOT(check()));
        // TODO track changes without timers
    }

    void start()
    {
        //t.start(5000);
        poll();
    }

    QList<Info> gateways() const
    {
        return info;
    }

    void poll()
    {
        info = get_unix_gateways();
    }

public slots:
    void check()
    {
        poll();
        emit updated();
    }
};

class UnixNetProvider : public IrisNetProvider
{
    Q_OBJECT
    Q_INTERFACES(XMPP::IrisNetProvider)
public:
    virtual NetGatewayProvider *createNetGatewayProvider()
    {
        return new UnixGateway;
    }
};

IrisNetProvider *irisnet_createUnixNetProvider()
{
    return new UnixNetProvider;
}

}

#include "netinterface_unix.moc"
