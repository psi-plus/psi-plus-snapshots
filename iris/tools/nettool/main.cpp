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

#include <QtCrypto>
#include <iris/addressresolver.h>
#include <iris/netavailability.h>
#include <iris/netinterface.h>
#include <iris/netnames.h>
#include <iris/processquit.h>
#include <iris/stunallocate.h>
#include <iris/stunbinding.h>
#include <iris/stunmessage.h>
#include <iris/stuntransaction.h>
#include <iris/turnclient.h>
#include <stdio.h>

using namespace XMPP;

static QString prompt(const QString &s)
{
    printf("* %s ", qPrintable(s));
    fflush(stdout);
    char    line[256];
    char *  ret = fgets(line, 255, stdin);
    QString result;
    if (ret)
        result = QString::fromLocal8Bit(line);
    if (result[result.length() - 1] == '\n')
        result.truncate(result.length() - 1);
    return result;
}

class NetMonitor : public QObject {
    Q_OBJECT
public:
    NetInterfaceManager * man;
    QList<NetInterface *> ifaces;
    NetAvailability *     netavail;

    ~NetMonitor()
    {
        delete netavail;
        qDeleteAll(ifaces);
        delete man;
    }

signals:
    void quit();

public slots:
    void start()
    {
        connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

        man = new NetInterfaceManager;
        connect(man, SIGNAL(interfaceAvailable(const QString &)), SLOT(here(const QString &)));
        QStringList list = man->interfaces();
        for (int n = 0; n < list.count(); ++n)
            here(list[n]);

        netavail = new NetAvailability;
        connect(netavail, SIGNAL(changed(bool)), SLOT(avail(bool)));
        avail(netavail->isAvailable());
    }

    void here(const QString &id)
    {
        NetInterface *iface = new NetInterface(id, man);
        connect(iface, SIGNAL(unavailable()), SLOT(gone()));
        printf("HERE: %s name=[%s]\n", qPrintable(iface->id()), qPrintable(iface->name()));
        QList<QHostAddress> addrs = iface->addresses();
        for (int n = 0; n < addrs.count(); ++n)
            printf("  address: %s\n", qPrintable(addrs[n].toString()));
        if (!iface->gateway().isNull())
            printf("  gateway: %s\n", qPrintable(iface->gateway().toString()));
        ifaces += iface;
    }

    void gone()
    {
        NetInterface *iface = static_cast<NetInterface *>(sender());
        printf("GONE: %s\n", qPrintable(iface->id()));
        ifaces.removeAll(iface);
        delete iface;
    }

    void avail(bool available)
    {
        if (available)
            printf("** Network available\n");
        else
            printf("** Network unavailable\n");
    }
};

static QString dataToString(const QByteArray &buf)
{
    QString out;
    for (int n = 0; n < buf.size(); ++n) {
        unsigned char c = (unsigned char)buf[n];
        if (c == '\\')
            out += "\\\\";
        else if (c >= 0x20 && c < 0x7f)
            out += c;
        else
            out += QString("\\x%1").arg((uint)c, 2, 16);
    }
    return out;
}

static void print_record(const NameRecord &r)
{
    switch (r.type()) {
    case NameRecord::A:
        printf("A: [%s] (ttl=%d)\n", qPrintable(r.address().toString()), r.ttl());
        break;
    case NameRecord::Aaaa:
        printf("AAAA: [%s] (ttl=%d)\n", qPrintable(r.address().toString()), r.ttl());
        break;
    case NameRecord::Mx:
        printf("MX: [%s] priority=%d (ttl=%d)\n", r.name().data(), r.priority(), r.ttl());
        break;
    case NameRecord::Srv:
        printf("SRV: [%s] port=%d priority=%d weight=%d (ttl=%d)\n", r.name().data(), r.port(), r.priority(),
               r.weight(), r.ttl());
        break;
    case NameRecord::Ptr:
        printf("PTR: [%s] (ttl=%d)\n", r.name().data(), r.ttl());
        break;
    case NameRecord::Txt: {
        QList<QByteArray> texts = r.texts();
        printf("TXT: count=%d (ttl=%d)\n", texts.count(), r.ttl());
        for (int n = 0; n < texts.count(); ++n)
            printf("  len=%d [%s]\n", texts[n].size(), qPrintable(dataToString(texts[n])));
        break;
    }
    case NameRecord::Hinfo:
        printf("HINFO: [%s] [%s] (ttl=%d)\n", r.cpu().data(), r.os().data(), r.ttl());
        break;
    case NameRecord::Null:
        printf("NULL: %d bytes (ttl=%d)\n", r.rawData().size(), r.ttl());
        break;
    default:
        printf("(Unknown): type=%d (ttl=%d)\n", r.type(), r.ttl());
        break;
    }
}

static int str2rtype(const QString &in)
{
    QString str = in.toLower();
    if (str == "a")
        return NameRecord::A;
    else if (str == "aaaa")
        return NameRecord::Aaaa;
    else if (str == "ptr")
        return NameRecord::Ptr;
    else if (str == "srv")
        return NameRecord::Srv;
    else if (str == "mx")
        return NameRecord::Mx;
    else if (str == "txt")
        return NameRecord::Txt;
    else if (str == "hinfo")
        return NameRecord::Hinfo;
    else if (str == "null")
        return NameRecord::Null;
    else
        return -1;
}

class ResolveName : public QObject {
    Q_OBJECT
public:
    ResolveName() = default;

    QString          name;
    NameRecord::Type type;
    bool             longlived = false;
    NameResolver     dns;
    bool             null_dump = false;

public slots:
    void start()
    {
        connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

        connect(&dns, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)),
                SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
        connect(&dns, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(dns_error(XMPP::NameResolver::Error)));

        dns.start(name.toLatin1(), type, longlived ? NameResolver::LongLived : NameResolver::Single);
    }

signals:
    void quit();

private slots:
    void dns_resultsReady(const QList<XMPP::NameRecord> &list)
    {
        if (null_dump && list[0].type() == NameRecord::Null) {
            QByteArray buf = list[0].rawData();
            if (fwrite(buf.data(), buf.size(), 1, stdout) != static_cast<size_t>(buf.size())) {
                /* FIXME:
                 C89/Unix allows fwrite() to return without having written everything.
                 The application should retry in that case.
                */
                fprintf(stderr, "Error: unable to write raw record to stdout\n");
                emit quit();
                return;
            }
        } else {
            for (int n = 0; n < list.count(); ++n)
                print_record(list[n]);
        }
        if (!longlived) {
            dns.stop();
            emit quit();
        }
    }

    void dns_error(XMPP::NameResolver::Error e)
    {
        QString str;
        if (e == NameResolver::ErrorNoName)
            str = "ErrorNoName";
        else if (e == NameResolver::ErrorTimeout)
            str = "ErrorTimeout";
        else if (e == NameResolver::ErrorNoLocal)
            str = "ErrorNoLocal";
        else if (e == NameResolver::ErrorNoLongLived)
            str = "ErrorNoLongLived";
        else // ErrorGeneric, or anything else
            str = "ErrorGeneric";

        printf("Error: %s\n", qPrintable(str));
        emit quit();
    }
};

class ResolveAddr : public QObject {
    Q_OBJECT
public:
    QString         name;
    AddressResolver dns;

public slots:
    void start()
    {
        connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

        connect(&dns, SIGNAL(resultsReady(const QList<QHostAddress> &)),
                SLOT(dns_resultsReady(const QList<QHostAddress> &)));
        connect(&dns, SIGNAL(error(XMPP::AddressResolver::Error)), SLOT(dns_error(XMPP::AddressResolver::Error)));

        dns.start(name.toLatin1());
    }

signals:
    void quit();

private slots:
    void dns_resultsReady(const QList<QHostAddress> &list)
    {
        for (int n = 0; n < list.count(); ++n)
            printf("%s\n", qPrintable(list[n].toString()));

        emit quit();
    }

    void dns_error(XMPP::AddressResolver::Error e)
    {
        Q_UNUSED(e);

        QString str;
        // else // ErrorGeneric, or anything else
        str = "ErrorGeneric";

        printf("Error: %s\n", qPrintable(str));
        emit quit();
    }
};

class BrowseServices : public QObject {
    Q_OBJECT
public:
    QString        type, domain;
    ServiceBrowser browser;

public slots:
    void start()
    {
        connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

        connect(&browser, SIGNAL(instanceAvailable(const XMPP::ServiceInstance &)),
                SLOT(browser_instanceAvailable(const XMPP::ServiceInstance &)));
        connect(&browser, SIGNAL(instanceUnavailable(const XMPP::ServiceInstance &)),
                SLOT(browser_instanceUnavailable(const XMPP::ServiceInstance &)));
        connect(&browser, SIGNAL(error()), SLOT(browser_error()));

        browser.start(type, domain);
    }

signals:
    void quit();

private slots:
    void browser_instanceAvailable(const XMPP::ServiceInstance &i)
    {
        printf("HERE: [%s] (%d attributes)\n", qPrintable(i.instance()), i.attributes().count());
        QMap<QString, QByteArray>         attribs = i.attributes();
        QMapIterator<QString, QByteArray> it(attribs);
        while (it.hasNext()) {
            it.next();
            printf("  [%s] = [%s]\n", qPrintable(it.key()), qPrintable(dataToString(it.value())));
        }
    }

    void browser_instanceUnavailable(const XMPP::ServiceInstance &i)
    {
        printf("GONE: [%s]\n", qPrintable(i.instance()));
    }

    void browser_error() { }
};

class ResolveService : public QObject {
    Q_OBJECT
public:
    int     mode;
    QString instance;
    QString type;
    QString domain;
    int     port;

    ServiceResolver dns;

public slots:
    void start()
    {
        connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

        connect(&dns, SIGNAL(resultsReady(const QHostAddress &, int)),
                SLOT(dns_resultsReady(const QHostAddress &, int)));
        connect(&dns, SIGNAL(finished()), SLOT(dns_finished()));
        connect(&dns, SIGNAL(error()), SLOT(dns_error()));

        if (mode == 0)
            dns.startFromInstance(instance.toLatin1() + '.' + type.toLatin1() + ".local.");
        else if (mode == 1)
            dns.startFromDomain(domain, type);
        else // 2
            dns.startFromPlain(domain, port);
    }

signals:
    void quit();

private slots:
    void dns_resultsReady(const QHostAddress &addr, int port)
    {
        printf("[%s] port=%d\n", qPrintable(addr.toString()), port);
        dns.tryNext();
    }

    void dns_finished() { emit quit(); }

    void dns_error()
    {
        printf("Error\n");
        emit quit();
    }
};

class PublishService : public QObject {
    Q_OBJECT
public:
    QString                   instance;
    QString                   type;
    int                       port;
    QMap<QString, QByteArray> attribs;
    QByteArray                extra_null;

    ServiceLocalPublisher pub;

public slots:
    void start()
    {
        // NetInterfaceManager::instance();

        connect(ProcessQuit::instance(), SIGNAL(quit()), SIGNAL(quit()));

        connect(&pub, SIGNAL(published()), SLOT(pub_published()));
        connect(&pub, SIGNAL(error(XMPP::ServiceLocalPublisher::Error)),
                SLOT(pub_error(XMPP::ServiceLocalPublisher::Error)));

        pub.publish(instance, type, port, attribs);
    }

signals:
    void quit();

private slots:
    void pub_published()
    {
        printf("Published\n");
        if (!extra_null.isEmpty()) {
            NameRecord rec;
            rec.setNull(extra_null);
            pub.addRecord(rec);
        }
    }

    void pub_error(XMPP::ServiceLocalPublisher::Error e)
    {
        printf("Error: [%d]\n", e);
        emit quit();
    }
};

class StunBind : public QObject {
    Q_OBJECT
public:
    bool                     debug;
    QHostAddress             addr;
    int                      port;
    int                      localPort;
    QUdpSocket *             sock;
    StunTransactionPool::Ptr pool;
    StunBinding *            binding;

    ~StunBind()
    {
        // make sure transactions are always deleted before the pool
        delete binding;
    }

public slots:
    void start()
    {
        sock = new QUdpSocket(this);
        connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));

        pool = new StunTransactionPool(StunTransaction::Udp, this);
        if (debug)
            pool->setDebugLevel(StunTransactionPool::DL_Packet);
        else
            pool->setDebugLevel(StunTransactionPool::DL_Info);
        connect(pool, SIGNAL(outgoingMessage(const QByteArray &, const QHostAddress &, int)),
                SLOT(pool_outgoingMessage(const QByteArray &, const QHostAddress &, int)));
        connect(pool, SIGNAL(debugLine(const QString &)), SLOT(pool_debugLine(const QString &)));

        if (!sock->bind(localPort != -1 ? localPort : 0)) {
            printf("Error binding to local port.\n");
            emit quit();
            return;
        }

        printf("Bound to local port %d.\n", sock->localPort());

        binding = new StunBinding(pool);
        connect(binding, SIGNAL(success()), SLOT(binding_success()));
        connect(binding, SIGNAL(error(XMPP::StunBinding::Error)), SLOT(binding_error(XMPP::StunBinding::Error)));
        binding->start();
    }

signals:
    void quit();

private slots:
    void sock_readyRead()
    {
        while (sock->hasPendingDatagrams()) {
            QByteArray   buf(sock->pendingDatagramSize(), 0);
            QHostAddress from;
            quint16      fromPort;

            sock->readDatagram(buf.data(), buf.size(), &from, &fromPort);
            if (from == addr && fromPort == port) {
                processDatagram(buf);
            } else {
                printf("Response from unknown sender %s:%d, dropping.\n", qPrintable(from.toString()), fromPort);
            }
        }
    }

    void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &toAddress, int toPort)
    {
        // in this example, we aren't using IP-associated transactions
        Q_UNUSED(toAddress);
        Q_UNUSED(toPort);

        sock->writeDatagram(packet, addr, port);
    }

    void pool_debugLine(const QString &line) { printf("%s\n", qPrintable(line)); }

    void binding_success()
    {
        QHostAddress saddr = binding->reflexiveAddress();
        quint16      sport = binding->reflexivePort();
        printf("Server says we are %s;%d\n", qPrintable(saddr.toString()), sport);
        emit quit();
    }

    void binding_error(XMPP::StunBinding::Error e)
    {
        Q_UNUSED(e);
        printf("Error: %s\n", qPrintable(binding->errorString()));
        emit quit();
    }

private:
    void processDatagram(const QByteArray &buf)
    {
        StunMessage message = StunMessage::fromBinary(buf);
        if (message.isNull()) {
            printf("Warning: server responded with what doesn't seem to be a STUN packet, skipping.\n");
            return;
        }

        if (!pool->writeIncomingMessage(message))
            printf("Warning: received unexpected message, skipping.\n");
    }
};

class TurnClientTest : public QObject {
    Q_OBJECT
public:
    int                      mode  = 0;
    bool                     debug = false;
    QHostAddress             relayAddr;
    int                      relayPort = 0;
    QString                  relayUser;
    QString                  relayPass;
    QString                  relayRealm;
    QHostAddress             peerAddr;
    int                      peerPort = 0;
    QUdpSocket *             udp      = nullptr;
    StunTransactionPool::Ptr pool;
    QList<bool>              writeItems; // true = turn-originated, false = external
    TurnClient *             turn = nullptr;

    TurnClientTest() = default;

    ~TurnClientTest()
    {
        // make sure transactions are always deleted before the pool
        delete turn;
    }

public slots:
    void start()
    {
        connect(ProcessQuit::instance(), SIGNAL(quit()), SLOT(do_quit()));

        turn = new TurnClient(this);
        if (debug)
            turn->setDebugLevel(TurnClient::DL_Packet);
        else
            turn->setDebugLevel(TurnClient::DL_Info);

        connect(turn, SIGNAL(connected()), SLOT(turn_connected()));
        connect(turn, SIGNAL(tlsHandshaken()), SLOT(turn_tlsHandshaken()));
        connect(turn, SIGNAL(closed()), SLOT(turn_closed()));
        connect(turn, SIGNAL(needAuthParams()), SLOT(turn_needAuthParams()));
        connect(turn, SIGNAL(retrying()), SLOT(turn_retrying()));
        connect(turn, SIGNAL(activated()), SLOT(turn_activated()));
        connect(turn, SIGNAL(readyRead()), SLOT(turn_readyRead()));
        connect(turn, SIGNAL(packetsWritten(int, const QHostAddress &, int)),
                SLOT(turn_packetsWritten(int, const QHostAddress &, int)));
        connect(turn, SIGNAL(error(XMPP::TurnClient::Error)), SLOT(turn_error(XMPP::TurnClient::Error)));
        connect(turn, SIGNAL(outgoingDatagram(const QByteArray &)), SLOT(turn_outgoingDatagram(const QByteArray &)));
        connect(turn, SIGNAL(debugLine(const QString &)), SLOT(turn_debugLine(const QString &)));

        turn->setClientSoftwareNameAndVersion("nettool (Iris)");

        if (mode == 0) {
            udp = new QUdpSocket(this);
            connect(udp, SIGNAL(readyRead()), SLOT(udp_readyRead()));

            // QUdpSocket bytesWritten is not DOR-DS safe, so we queue
            connect(udp, SIGNAL(bytesWritten(qint64)), SLOT(udp_bytesWritten(qint64)), Qt::QueuedConnection);

            pool = new StunTransactionPool(StunTransaction::Udp, this);
            if (debug)
                pool->setDebugLevel(StunTransactionPool::DL_Packet);
            else
                pool->setDebugLevel(StunTransactionPool::DL_Info);
            connect(pool, SIGNAL(outgoingMessage(const QByteArray &, const QHostAddress &, int)),
                    SLOT(pool_outgoingMessage(const QByteArray &, const QHostAddress &, int)));
            connect(pool, SIGNAL(needAuthParams()), SLOT(pool_needAuthParams()));
            connect(pool, SIGNAL(debugLine(const QString &)), SLOT(pool_debugLine(const QString &)));

            pool->setLongTermAuthEnabled(true);
            if (!relayUser.isEmpty()) {
                pool->setUsername(relayUser);
                pool->setPassword(relayPass.toUtf8());
                if (!relayRealm.isEmpty())
                    pool->setRealm(relayRealm);
            }

            if (!udp->bind()) {
                printf("Error binding to local port.\n");
                emit quit();
                return;
            }

            turn->connectToHost(pool);
        } else {
            if (!relayUser.isEmpty()) {
                turn->setUsername(relayUser);
                turn->setPassword(relayPass.toUtf8());
                if (!relayRealm.isEmpty())
                    turn->setRealm(relayRealm);
            }

            printf("TCP connecting...\n");
            turn->connectToHost(relayAddr, relayPort, mode == 2 ? TurnClient::TlsMode : TurnClient::PlainMode);
        }
    }

signals:
    void quit();

private:
    void processDatagram(const QByteArray &buf)
    {
        QByteArray   data;
        QHostAddress fromAddr;
        int          fromPort;

        bool notStun;
        if (!pool->writeIncomingMessage(buf, &notStun)) {
            data = turn->processIncomingDatagram(buf, notStun, &fromAddr, &fromPort);
            if (!data.isNull())
                processDataPacket(data, fromAddr, fromPort);
            else
                printf("Warning: server responded with what doesn't seem to be a STUN or data packet, skipping.\n");
        }
    }

    void processDataPacket(const QByteArray &buf, const QHostAddress &addr, int port)
    {
        printf("Received %d bytes from %s:%d: [%s]\n", buf.size(), qPrintable(addr.toString()), port, buf.data());

        turn->close();
    }

private slots:
    void do_quit()
    {
        ProcessQuit::cleanup();

        turn->close();
    }

    void udp_readyRead()
    {
        while (udp->hasPendingDatagrams()) {
            QByteArray   buf(udp->pendingDatagramSize(), 0);
            QHostAddress from;
            quint16      fromPort;

            udp->readDatagram(buf.data(), buf.size(), &from, &fromPort);
            if (from == relayAddr && fromPort == relayPort) {
                processDatagram(buf);
            } else {
                printf("Response from unknown sender %s:%d, dropping.\n", qPrintable(from.toString()), fromPort);
            }
        }
    }

    void udp_bytesWritten(qint64 bytes)
    {
        Q_UNUSED(bytes);
        bool wasTurnOriginated = writeItems.takeFirst();
        if (wasTurnOriginated)
            turn->outgoingDatagramsWritten(1);
    }

    void pool_outgoingMessage(const QByteArray &packet, const QHostAddress &toAddress, int toPort)
    {
        // in this example, we aren't using IP-associated transactions
        Q_UNUSED(toAddress);
        Q_UNUSED(toPort);

        writeItems += false;
        udp->writeDatagram(packet, relayAddr, relayPort);
    }

    void pool_needAuthParams()
    {
        relayUser = prompt("Username:");
        relayPass = prompt("Password:");

        pool->setUsername(relayUser);
        pool->setPassword(relayPass.toUtf8());

        QString str = prompt(QString("Realm: [%1]").arg(pool->realm()));
        if (!str.isEmpty()) {
            relayRealm = str;
            pool->setRealm(relayRealm);
        } else
            relayRealm = pool->realm();

        pool->continueAfterParams();
    }

    void pool_debugLine(const QString &line) { turn_debugLine(line); }

    void turn_connected() { printf("TCP connected\n"); }

    void turn_tlsHandshaken() { printf("TLS handshake completed\n"); }

    void turn_closed()
    {
        printf("Done\n");
        emit quit();
    }

    void turn_needAuthParams()
    {
        relayUser = prompt("Username:");
        relayPass = prompt("Password:");

        turn->setUsername(relayUser);
        turn->setPassword(relayPass.toUtf8());

        QString str = prompt(QString("Realm: [%1]").arg(turn->realm()));
        if (!str.isEmpty()) {
            relayRealm = str;
            turn->setRealm(relayRealm);
        } else
            relayRealm = turn->realm();

        turn->continueAfterParams();
    }

    void turn_retrying() { printf("Mismatch error, retrying...\n"); }

    void turn_activated()
    {
        StunAllocate *allocate = turn->stunAllocate();

        QHostAddress saddr = allocate->reflexiveAddress();
        quint16      sport = allocate->reflexivePort();
        printf("Server says we are %s;%d\n", qPrintable(saddr.toString()), sport);
        saddr = allocate->relayedAddress();
        sport = allocate->relayedPort();
        printf("Server relays via %s;%d\n", qPrintable(saddr.toString()), sport);

        // optional: flag this destination to use a channelbind
        turn->addChannelPeer(peerAddr, peerPort);

        QByteArray buf = "Hello, world!";
        printf("Relaying test packet of %d bytes [%s] to %s;%d...\n", buf.size(), buf.data(),
               qPrintable(peerAddr.toString()), peerPort);
        turn->write(buf, peerAddr, peerPort);
    }

    void turn_readyRead()
    {
        QHostAddress addr;
        int          port;
        QByteArray   buf = turn->read(&addr, &port);

        processDataPacket(buf, addr, port);
    }

    void turn_packetsWritten(int count, const QHostAddress &addr, int port)
    {
        Q_UNUSED(addr);
        Q_UNUSED(port);

        printf("%d packet(s) written\n", count);
    }

    void turn_error(XMPP::TurnClient::Error e)
    {
        Q_UNUSED(e);
        printf("Error: %s\n", qPrintable(turn->errorString()));
        emit quit();
    }

    void turn_outgoingDatagram(const QByteArray &buf)
    {
        writeItems += true;
        udp->writeDatagram(buf, relayAddr, relayPort);
    }

    void turn_debugLine(const QString &line) { printf("%s\n", qPrintable(line)); }
};

void usage()
{
    printf("nettool: simple testing utility\n");
    printf("usage: nettool (options) [command]\n");
    printf("  options: --debug, --user=x, --pass=x, --realm=x\n");
    printf("\n");
    printf(" netmon                                            monitor network interfaces\n");
    printf(" rname (-r) [domain] (record type)                 look up record (default = a)\n");
    printf(" rnamel [domain] [record type]                     look up record (long-lived)\n");
    printf(" raddr [domain]                                    look up AAAA/A\n");
    printf(" browse [service type]                             browse for local services\n");
    printf(" rservi [instance] [service type]                  look up browsed instance\n");
    printf(" rservd [domain] [service type]                    look up normal SRV\n");
    printf(" rservp [domain] [port]                            look up non-SRV\n");
    printf(" pserv [inst] [type] [port] (attr) (-a [rec])      publish service instance\n");
    printf(" stun [addr](;port) (local port)                   STUN binding\n");
    printf(" turn [mode] [relayaddr](;port) [peeraddr](;port)  TURN UDP echo test\n");
    printf("\n");
    printf("record types: a aaaa ptr srv mx txt hinfo null\n");
    printf("service types: _service._proto format (e.g. \"_xmpp-client._tcp\")\n");
    printf("attributes: var0[=val0],...,varn[=valn]\n");
    printf("rname -r: for null type, dump raw record data to stdout\n");
    printf("pserv -a: add extra record.  format: null:filename.dat\n");
    printf("turn modes: udp tcp tcp-tls\n");
    printf("\n");
}

int main(int argc, char **argv)
{
    QCA::Initializer qcaInit;
    QCoreApplication qapp(argc, argv);

    QStringList args = qapp.arguments();
    args.removeFirst();

    QString user, pass, realm;
    bool    debug = false;

    for (int n = 0; n < args.count(); ++n) {
        QString s = args[n];
        if (!s.startsWith("--"))
            continue;
        QString var;
        QString val;
        int     x = s.indexOf('=');
        if (x != -1) {
            var = s.mid(2, x - 2);
            val = s.mid(x + 1);
        } else {
            var = s.mid(2);
        }

        bool known = true;

        if (var == "debug")
            debug = true;
        else if (var == "user")
            user = val;
        else if (var == "pass")
            pass = val;
        else if (var == "realm")
            realm = val;
        else
            known = false;

        if (!known) {
            fprintf(stderr, "Unknown option '%s'.\n", qPrintable(var));
            return 1;
        }

        args.removeAt(n);
        --n; // adjust position
    }

    if (args.isEmpty()) {
        usage();
        return 1;
    }

    if (args[0] == "netmon") {
        NetMonitor a;
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "rname" || args[0] == "rnamel") {
        bool null_dump = false;
        for (int n = 1; n < args.count(); ++n) {
            if (args[n] == "-r") {
                null_dump = true;
                args.removeAt(n);
                --n;
            }
        }

        if (args.count() < 2) {
            usage();
            return 1;
        }
        if (args[0] == "rnamel" && args.count() < 3) {
            usage();
            return 1;
        }
        int x = NameRecord::A;
        if (args.count() >= 3) {
            x = str2rtype(args[2]);
            if (x == -1) {
                usage();
                return 1;
            }
        }
        ResolveName a;
        a.name      = args[1];
        a.type      = (NameRecord::Type)x;
        a.longlived = args[0] == "rnamel";
        if (args[0] == "rname" && null_dump)
            a.null_dump = true;
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "raddr") {
        if (args.count() < 2) {
            usage();
            return 1;
        }

        ResolveAddr a;
        a.name = args[1];
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "browse") {
        if (args.count() < 2) {
            usage();
            return 1;
        }

        BrowseServices a;
        a.type = args[1];
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "rservi" || args[0] == "rservd" || args[0] == "rservp") {
        // they all take 2 params
        if (args.count() < 3) {
            usage();
            return 1;
        }

        ResolveService a;
        if (args[0] == "rservi") {
            a.mode     = 0;
            a.instance = args[1];
            a.type     = args[2];
        } else if (args[0] == "rservd") {
            a.mode   = 1;
            a.domain = args[1];
            a.type   = args[2];
        } else // rservp
        {
            a.mode   = 2;
            a.domain = args[1];
            a.port   = args[2].toInt();
        }
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "pserv") {
        QStringList addrecs;
        for (int n = 1; n < args.count(); ++n) {
            if (args[n] == "-a") {
                if (n + 1 < args.count()) {
                    addrecs += args[n + 1];
                    args.removeAt(n);
                    args.removeAt(n);
                    --n;
                } else {
                    usage();
                    return 1;
                }
            }
        }

        QByteArray extra_null;
        for (int n = 0; n < addrecs.count(); ++n) {
            const QString &str = addrecs[n];
            int            x   = str.indexOf(':');
            if (x == -1 || str.mid(0, x) != "null") {
                usage();
                return 1;
            }

            QString null_file = str.mid(x + 1);

            if (!null_file.isEmpty()) {
                QFile f(null_file);
                if (!f.open(QFile::ReadOnly)) {
                    printf("can't read file\n");
                    return 1;
                }
                extra_null = f.readAll();
            }
        }

        if (args.count() < 4) {
            usage();
            return 1;
        }

        QMap<QString, QByteArray> attribs;
        if (args.count() > 4) {
            QStringList parts = args[4].split(',');
            for (int n = 0; n < parts.count(); ++n) {
                const QString &str = parts[n];
                int            x   = str.indexOf('=');
                if (x != -1)
                    attribs.insert(str.mid(0, x), str.mid(x + 1).toUtf8());
                else
                    attribs.insert(str, QByteArray());
            }
        }

        PublishService a;
        a.instance   = args[1];
        a.type       = args[2];
        a.port       = args[3].toInt();
        a.attribs    = attribs;
        a.extra_null = extra_null;
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "stun") {
        if (args.count() < 2) {
            usage();
            return 1;
        }

        QString addrstr, portstr;
        int     x = args[1].indexOf(';');
        if (x != -1) {
            addrstr = args[1].mid(0, x);
            portstr = args[1].mid(x + 1);
        } else
            addrstr = args[1];

        QHostAddress addr = QHostAddress(addrstr);
        if (addr.isNull()) {
            printf("Error: addr must be an IP address\n");
            return 1;
        }

        int port = 3478;
        if (!portstr.isEmpty())
            port = portstr.toInt();

        int localPort = -1;
        if (args.count() >= 3)
            localPort = args[2].toInt();

        if (!QCA::isSupported("hmac(sha1)")) {
            printf("Error: Need hmac(sha1) support to use STUN.\n");
            return 1;
        }

        StunBind a;
        a.debug     = debug;
        a.localPort = localPort;
        a.addr      = addr;
        a.port      = port;
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else if (args[0] == "turn") {
        if (args.count() < 4) {
            usage();
            return 1;
        }

        int mode;
        if (args[1] == "udp")
            mode = 0;
        else if (args[1] == "tcp")
            mode = 1;
        else if (args[1] == "tcp-tls")
            mode = 2;
        else {
            usage();
            return 1;
        }

        QString addrstr, portstr;
        int     x = args[2].indexOf(';');
        if (x != -1) {
            addrstr = args[2].mid(0, x);
            portstr = args[2].mid(x + 1);
        } else
            addrstr = args[2];

        QHostAddress raddr = QHostAddress(addrstr);
        if (raddr.isNull()) {
            printf("Error: relayaddr must be an IP address\n");
            return 1;
        }

        int rport = 3478;
        if (!portstr.isEmpty())
            rport = portstr.toInt();

        portstr.clear();
        x = args[3].indexOf(';');
        if (x != -1) {
            addrstr = args[3].mid(0, x);
            portstr = args[3].mid(x + 1);
        } else
            addrstr = args[3];

        QHostAddress paddr = QHostAddress(addrstr);
        if (raddr.isNull()) {
            printf("Error: peeraddr must be an IP address\n");
            return 1;
        }

        int pport = 4588;
        if (!portstr.isEmpty())
            pport = portstr.toInt();

        if (!QCA::isSupported("hmac(sha1)")) {
            printf("Error: Need hmac(sha1) support to use TURN.\n");
            return 1;
        }

        if (mode == 2 && !QCA::isSupported("tls")) {
            printf("Error: Need tls support to use tcp-tls mode.\n");
            return 1;
        }

        TurnClientTest a;
        a.mode       = mode;
        a.debug      = debug;
        a.relayAddr  = raddr;
        a.relayPort  = rport;
        a.relayUser  = user;
        a.relayPass  = pass;
        a.relayRealm = realm;
        a.peerAddr   = paddr;
        a.peerPort   = pport;
        QObject::connect(&a, SIGNAL(quit()), &qapp, SLOT(quit()));
        QTimer::singleShot(0, &a, SLOT(start()));
        qapp.exec();
    } else {
        usage();
        return 1;
    }
    return 0;
}

#include "main.moc"
