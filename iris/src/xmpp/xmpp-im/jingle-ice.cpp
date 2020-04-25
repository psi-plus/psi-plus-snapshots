/*
 * jignle-s5b.cpp - Jingle SOCKS5 transport
 * Copyright (C) 2019  Sergey Ilinykh
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

#include "jingle-ice.h"

#include "ice176.h"
#include "jingle-session.h"
#include "netnames.h"
#include "udpportreserver.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_serverinfomanager.h"

#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTimer>

namespace XMPP { namespace Jingle { namespace ICE {
    const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ice:0"));

    // TODO: reject offers that don't contain at least one of audio or video
    // TODO: support candidate negotiations over the JingleRtpChannel thread
    //   boundary, so we can change candidates after the stream is active

    // scope values: 0 = local, 1 = link-local, 2 = private, 3 = public
    static int getAddressScope(const QHostAddress &a)
    {
        if (a.protocol() == QAbstractSocket::IPv6Protocol) {
            if (a == QHostAddress(QHostAddress::LocalHostIPv6))
                return 0;
            else if (XMPP::Ice176::isIPv6LinkLocalAddress(a))
                return 1;
        } else if (a.protocol() == QAbstractSocket::IPv4Protocol) {
            quint32 v4 = a.toIPv4Address();
            quint8  a0 = v4 >> 24;
            quint8  a1 = (v4 >> 16) & 0xff;
            if (a0 == 127)
                return 0;
            else if (a0 == 169 && a1 == 254)
                return 1;
            else if (a0 == 10)
                return 2;
            else if (a0 == 172 && a1 >= 16 && a1 <= 31)
                return 2;
            else if (a0 == 192 && a1 == 168)
                return 2;
        }

        return 3;
    }

    // -1 = a is higher priority, 1 = b is higher priority, 0 = equal
    static int comparePriority(const QHostAddress &a, const QHostAddress &b)
    {
        // prefer closer scope
        int a_scope = getAddressScope(a);
        int b_scope = getAddressScope(b);
        if (a_scope < b_scope)
            return -1;
        else if (a_scope > b_scope)
            return 1;

        // prefer ipv6
        if (a.protocol() == QAbstractSocket::IPv6Protocol && b.protocol() != QAbstractSocket::IPv6Protocol)
            return -1;
        else if (b.protocol() == QAbstractSocket::IPv6Protocol && a.protocol() != QAbstractSocket::IPv6Protocol)
            return 1;

        return 0;
    }

    static QList<QHostAddress> sortAddrs(const QList<QHostAddress> &in)
    {
        QList<QHostAddress> out;

        for (const QHostAddress &a : in) {
            int at;
            for (at = 0; at < out.count(); ++at) {
                if (comparePriority(a, out[at]) < 0)
                    break;
            }

            out.insert(at, a);
        }

        return out;
    }

    // resolve external address and stun server
    // TODO: resolve hosts and start ice engine simultaneously
    // FIXME: when/if our ICE engine supports adding these dynamically, we should
    //   not have the lookups block on each other
    class Resolver : public QObject {
        Q_OBJECT

    private:
        XMPP::NameResolver dnsA;
        XMPP::NameResolver dnsB;
        XMPP::NameResolver dnsC;
        XMPP::NameResolver dnsD;
        QString            extHost;
        QString            stunBindHost, stunRelayUdpHost, stunRelayTcpHost;
        bool               extDone;
        bool               stunBindDone;
        bool               stunRelayUdpDone;
        bool               stunRelayTcpDone;

    public:
        QHostAddress extAddr;
        QHostAddress stunBindAddr, stunRelayUdpAddr, stunRelayTcpAddr;

        Resolver(QObject *parent = nullptr) : QObject(parent), dnsA(parent), dnsB(parent), dnsC(parent), dnsD(parent)
        {
            connect(&dnsA, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)),
                    SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
            connect(&dnsA, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(dns_error(XMPP::NameResolver::Error)));

            connect(&dnsB, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)),
                    SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
            connect(&dnsB, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(dns_error(XMPP::NameResolver::Error)));

            connect(&dnsC, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)),
                    SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
            connect(&dnsC, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(dns_error(XMPP::NameResolver::Error)));

            connect(&dnsD, SIGNAL(resultsReady(const QList<XMPP::NameRecord> &)),
                    SLOT(dns_resultsReady(const QList<XMPP::NameRecord> &)));
            connect(&dnsD, SIGNAL(error(XMPP::NameResolver::Error)), SLOT(dns_error(XMPP::NameResolver::Error)));
        }

        void start(const QString &_extHost, const QString &_stunBindHost, const QString &_stunRelayUdpHost,
                   const QString &_stunRelayTcpHost)
        {
            extHost          = _extHost;
            stunBindHost     = _stunBindHost;
            stunRelayUdpHost = _stunRelayUdpHost;
            stunRelayTcpHost = _stunRelayTcpHost;

            if (!extHost.isEmpty()) {
                extDone = false;
                dnsA.start(extHost.toLatin1());
            } else
                extDone = true;

            if (!stunBindHost.isEmpty()) {
                stunBindDone = false;
                dnsB.start(stunBindHost.toLatin1());
            } else
                stunBindDone = true;

            if (!stunRelayUdpHost.isEmpty()) {
                stunRelayUdpDone = false;
                dnsC.start(stunRelayUdpHost.toLatin1());
            } else
                stunRelayUdpDone = true;

            if (!stunRelayTcpHost.isEmpty()) {
                stunRelayTcpDone = false;
                dnsD.start(stunRelayTcpHost.toLatin1());
            } else
                stunRelayTcpDone = true;

            if (extDone && stunBindDone && stunRelayUdpDone && stunRelayTcpDone)
                QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
        }

    signals:
        void finished();

    private slots:
        void dns_resultsReady(const QList<XMPP::NameRecord> &results)
        {
            XMPP::NameResolver *dns = static_cast<XMPP::NameResolver *>(sender());

            // FIXME: support more than one address?
            QHostAddress addr = results.first().address();

            if (dns == &dnsA) {
                extAddr = addr;
                extDone = true;
                tryFinish();
            } else if (dns == &dnsB) {
                stunBindAddr = addr;
                stunBindDone = true;
                tryFinish();
            } else if (dns == &dnsC) {
                stunRelayUdpAddr = addr;
                stunRelayUdpDone = true;
                tryFinish();
            } else // dnsD
            {
                stunRelayTcpAddr = addr;
                stunRelayTcpDone = true;
                tryFinish();
            }
        }

        void dns_error(XMPP::NameResolver::Error e)
        {
            Q_UNUSED(e)

            XMPP::NameResolver *dns = static_cast<XMPP::NameResolver *>(sender());

            if (dns == &dnsA) {
                extDone = true;
                tryFinish();
            } else if (dns == &dnsB) {
                stunBindDone = true;
                tryFinish();
            } else if (dns == &dnsC) {
                stunRelayUdpDone = true;
                tryFinish();
            } else // dnsD
            {
                stunRelayTcpDone = true;
                tryFinish();
            }
        }

    private:
        void tryFinish()
        {
            if (extDone && stunBindDone && stunRelayUdpDone && stunRelayTcpDone)
                emit finished();
        }
    };

    class IceStopper : public QObject {
        Q_OBJECT

    public:
        QTimer                 t;
        XMPP::UdpPortReserver *portReserver;
        QList<XMPP::Ice176 *>  left;

        IceStopper(QObject *parent = nullptr) : QObject(parent), t(this), portReserver(nullptr)
        {
            connect(&t, SIGNAL(timeout()), SLOT(t_timeout()));
            t.setSingleShot(true);
        }

        ~IceStopper()
        {
            qDeleteAll(left);
            delete portReserver;
            printf("IceStopper done\n");
        }

        void start(XMPP::UdpPortReserver *_portReserver, const QList<XMPP::Ice176 *> iceList)
        {
            if (_portReserver) {
                portReserver = _portReserver;
                portReserver->setParent(this);
            }
            left = iceList;

            for (XMPP::Ice176 *ice : left) {
                ice->setParent(this);

                // TODO: error() also?
                connect(ice, SIGNAL(stopped()), SLOT(ice_stopped()));
                connect(ice, SIGNAL(error(XMPP::Ice176::Error)), SLOT(ice_error(XMPP::Ice176::Error)));
                ice->stop();
            }

            t.start(3000);
        }

    private slots:
        void ice_stopped()
        {
            XMPP::Ice176 *ice = static_cast<XMPP::Ice176 *>(sender());
            ice->disconnect(this);
            ice->setParent(nullptr);
            ice->deleteLater();
            left.removeAll(ice);
            if (left.isEmpty())
                deleteLater();
        }

        void ice_error(XMPP::Ice176::Error e)
        {
            Q_UNUSED(e)

            ice_stopped();
        }

        void t_timeout() { deleteLater(); }
    };

    class Manager::Private {
    public:
        XMPP::Jingle::Manager *jingleManager = nullptr;

        int          basePort = -1;
        QString      extHost;
        QHostAddress selfAddr;

        QString stunBindHost;
        int     stunBindPort;
        QString stunRelayUdpHost;
        int     stunRelayUdpPort;
        QString stunRelayUdpUser;
        QString stunRelayUdpPass;
        QString stunRelayTcpHost;
        int     stunRelayTcpPort;
        QString stunRelayTcpUser;
        QString stunRelayTcpPass;

        XMPP::TurnClient::Proxy stunProxy;

        // FIMME it's reuiqred to split transports by direction otherwise we gonna hit conflicts.
        // jid,transport-sid -> transport mapping
        //        QSet<QPair<Jid, QString>>   sids;
        //        QHash<QString, Transport *> key2transport;
        //        Jid                         proxy;
    };

    class JingleRtpRemoteCandidate {
    public:
        int          component;
        QHostAddress addr;
        int          port;

        JingleRtpRemoteCandidate() : component(-1), port(-1) { }
    };

    class JingleRtpTrans {
    public:
        QString                         user;
        QString                         pass;
        QList<XMPP::Ice176::Candidate>  candidates;
        QList<JingleRtpRemoteCandidate> remoteCandidates;
    };

    static XMPP::Ice176::Candidate elementToCandidate(const QDomElement &e)
    {
        if (e.tagName() != "candidate")
            return XMPP::Ice176::Candidate();

        XMPP::Ice176::Candidate c;
        c.component  = e.attribute("component").toInt();
        c.foundation = e.attribute("foundation");
        c.generation = e.attribute("generation").toInt();
        c.id         = e.attribute("id");
        c.ip         = QHostAddress(e.attribute("ip"));
        c.network    = e.attribute("network").toInt();
        c.port       = e.attribute("port").toInt();
        c.priority   = e.attribute("priority").toInt();
        c.protocol   = e.attribute("protocol");
        c.rel_addr   = QHostAddress(e.attribute("rel-addr"));
        c.rel_port   = e.attribute("rel-port").toInt();
        // TODO: remove this?
        // c.rem_addr = QHostAddress(e.attribute("rem-addr"));
        // c.rem_port = e.attribute("rem-port").toInt();
        c.type = e.attribute("type");
        return c;
    }

    class Connection : public XMPP::Jingle::Connection {
        Q_OBJECT
    public:
        QList<NetworkDatagram> datagrams;
        void *                 client;
        int                    channelIndex;

        Connection(int channelIndex) : channelIndex(channelIndex)
        {
            /*connect(client, &SocksClient::readyRead, this, &Connection::readyRead);
            connect(client, &SocksClient::bytesWritten, this, &Connection::bytesWritten);
            connect(client, &SocksClient::aboutToClose, this, &Connection::aboutToClose);
            if (client->isOpen()) {
                setOpenMode(client->openMode());
            } else {
                qWarning("Creating S5B Transport connection on closed SockClient connection %p", client);
            }*/
        }

        bool hasPendingDatagrams() const { return datagrams.size() > 0; }

        NetworkDatagram receiveDatagram(qint64 maxSize = -1)
        {
            Q_UNUSED(maxSize) // TODO or not?
            return datagrams.size() ? datagrams.takeFirst() : NetworkDatagram();
        }

        qint64 bytesAvailable() const { return 0; }

        qint64 bytesToWrite() const { return 0; /*client->bytesToWrite();*/ }

        void close() { XMPP::Jingle::Connection::close(); }

    protected:
        qint64 writeData(const char *data, qint64 maxSize)
        {
            return 0; // client->write(data, maxSize);
        }

        qint64 readData(char *data, qint64 maxSize) { return 0; }

    private:
        friend class Transport;

        void onConnected(Ice176 *ice) { emit connected(); }

        void enqueueIncomingUDP(const QByteArray &data)
        {
            datagrams.append(NetworkDatagram { data });
            emit readyRead();
        }
    };

    class Transport::Private {
    public:
        enum PendingActions { NewCandidate = 1, RemoteCandidate = 2, GatheringComplete = 4 };

        Transport *                    q                               = nullptr;
        bool                           offerSent                       = false;
        bool                           waitingAck                      = true;
        bool                           aborted                         = false;
        bool                           proxyDiscoveryInProgress        = false; // if we have valid proxy requests
        bool                           remoteReportedGatheringComplete = false;
        bool                           iceStarted                      = false;
        quint16                        pendingActions                  = 0;
        int                            proxiesInDiscoCount             = 0;
        QList<XMPP::Ice176::Candidate> localCandidates; // cid to candidate mapping
        QList<XMPP::Ice176::Candidate> remoteCandidates;
        QString                        remoteUfrag;
        QString                        remotePassword;

        // QString            sid;
        // Transport::Mode    mode = Transport::Tcp;
        // QTimer             probingTimer;
        // QTimer             negotiationFinishTimer;
        // QElapsedTimer      lastConnectionStart;
        // size_t             blockSize    = 8192;
        TcpPortDiscoverer *disco        = nullptr;
        UdpPortReserver *  portReserver = nullptr;
        Resolver           resolver;
        XMPP::Ice176 *     ice = nullptr;

        QHostAddress extAddr;
        QHostAddress stunBindAddr, stunRelayUdpAddr, stunRelayTcpAddr;
        int          stunBindPort;
        int          stunRelayUdpPort;
        int          stunRelayTcpPort;

        QMap<int, QSharedPointer<Connection>> channels;

        // udp stuff
        bool         udpInitialized;
        quint16      udpPort;
        QHostAddress udpAddress;

        inline Jid remoteJid() const { return q->_pad->session()->peer(); }

        void flushRemoteCandidates()
        {
            if (q->_state < State::ApprovedToSend || q->_state == State::Finished)
                return;
            ice->setPeerUfrag(remoteUfrag);
            ice->setPeerPassword(remotePassword);
            if (!remoteCandidates.isEmpty()) {
                ice->addRemoteCandidates(remoteCandidates);
                remoteCandidates.clear();
            }
        }

        bool handleIncomingCandidate(const QDomElement &transportEl)
        {
            QString candidateTag(QStringLiteral("candidate"));
            int     candidatesAdded = 0;
            for (QDomElement ce = transportEl.firstChildElement(candidateTag); !ce.isNull();
                 ce             = ce.nextSiblingElement(candidateTag)) {
                XMPP::Ice176::Candidate ic = elementToCandidate(ce);
                if (ic.type.isEmpty()) {
                    throw std::runtime_error("failed to parse incoming candidate");
                }
                if (!candidatesAdded) {
                    remoteUfrag    = transportEl.attribute("ufrag");
                    remotePassword = transportEl.attribute("pwd");
                    if (remoteUfrag.isEmpty() || remotePassword.isEmpty())
                        throw std::runtime_error("user fragment or password can't be empty");
                }
                // qDebug("new remote candidate: %s", qPrintable(c.toString()));
                remoteCandidates.append(ic); // TODO check for collisions!
                candidatesAdded++;
            }
            if (candidatesAdded) {
                QTimer::singleShot(0, q, [this]() { flushRemoteCandidates(); });
                return true;
            }
            return false;
        }

        bool handleIncomingRemoteCandidate(const QDomElement &transportEl)
        {
            QDomElement el = transportEl.firstChildElement(QStringLiteral("remote-candidate"));
            if (!el.isNull()) {
                bool     ok, ok2;
                auto     component = el.attribute(QLatin1String("component")).toUInt(&ok);
                auto     ip        = QHostAddress(el.attribute(QLatin1String("ip")));
                uint16_t port      = el.attribute(QLatin1String("port")).toUShort(&ok2);

                if (!(ok && ok2 && !ip.isNull()))
                    throw std::runtime_error("failed to parse remote-candidate");
                /*
                                auto cUsed = localCandidates.value(el.attribute(QStringLiteral("cid")));
                                if (!cUsed) {
                                    throw std::runtime_error("failed to find incoming candidate-used candidate");
                                }
                                if (cUsed.state() == Candidate::Pending) {
                                    cUsed.setState(Candidate::Accepted);
                                    localUsedCandidate = cUsed;
                                    updateMinimalPriorityOnConnected();
                                    QTimer::singleShot(0, q, [this]() { checkAndFinishNegotiation(); });
                                } else {
                                    // we already rejected the candidate and either remote side already knows about it
                   or will soon
                                    // it's possible for example if we were able to connect to higher priority
                   candidate, so
                                    // we have o pretend like remote couldn't select anything better but finished
                   already, in other
                                    // words like if it sent candidate-error.
                                    localUsedCandidate           = Candidate();
                                }
                                */
                return true;
            }
            return false;
        }

        bool handleIncomingGatheringComplete(const QDomElement &transportEl)
        {
            auto el = transportEl.firstChildElement(QStringLiteral("gathering-complete"));
            if (!el.isNull()) {
                remoteReportedGatheringComplete = true;
                /*
                for (auto &c : localCandidates) {
                    if (c.state() == Candidate::Pending) {
                        c.setState(Candidate::Discarded);
                    }
                }
                */
                qDebug("recv gathering-complete: all local pending candidates were discarded");
                // QTimer::singleShot(0, q, [this]() { checkAndFinishNegotiation(); });
                return true;
            }
            return false;
        }

        void startIce()
        {
            auto manager = dynamic_cast<Manager *>(q->_pad->manager())->d.data();

            stunBindPort     = manager->stunBindPort;
            stunRelayUdpPort = manager->stunRelayUdpPort;
            stunRelayTcpPort = manager->stunRelayTcpPort;
            if (!stunBindAddr.isNull() && stunBindPort > 0)
                printf("STUN service: %s;%d\n", qPrintable(stunBindAddr.toString()), stunBindPort);
            if (!stunRelayUdpAddr.isNull() && stunRelayUdpPort > 0 && !manager->stunRelayUdpUser.isEmpty())
                printf("TURN w/ UDP service: %s;%d\n", qPrintable(stunRelayUdpAddr.toString()), stunRelayUdpPort);
            if (!stunRelayTcpAddr.isNull() && stunRelayTcpPort > 0 && !manager->stunRelayTcpUser.isEmpty())
                printf("TURN w/ TCP service: %s;%d\n", qPrintable(stunRelayTcpAddr.toString()), stunRelayTcpPort);

            QList<QHostAddress> listenAddrs;
            auto const          interfaces = QNetworkInterface::allInterfaces();
            for (const QNetworkInterface &ni : interfaces) {
                QList<QNetworkAddressEntry> entries = ni.addressEntries();
                for (const QNetworkAddressEntry &na : entries) {
                    QHostAddress h = na.ip();

                    // skip localhost
                    if (getAddressScope(h) == 0)
                        continue;

                    // don't put the same address in twice.
                    //   this also means that if there are
                    //   two link-local ipv6 interfaces
                    //   with the exact same address, we
                    //   only use the first one
                    if (listenAddrs.contains(h))
                        continue;

                    if (h.protocol() == QAbstractSocket::IPv6Protocol && XMPP::Ice176::isIPv6LinkLocalAddress(h))
                        h.setScopeId(ni.name());
                    listenAddrs += h;
                }
            }

            listenAddrs = sortAddrs(listenAddrs);

            QList<XMPP::Ice176::LocalAddress> localAddrs;

            QStringList strList;
            for (const QHostAddress &h : listenAddrs) {
                XMPP::Ice176::LocalAddress addr;
                addr.addr = h;
                localAddrs += addr;
                strList += h.toString();
            }

            if (manager->basePort != -1) {
                portReserver = new XMPP::UdpPortReserver(q);
                portReserver->setAddresses(listenAddrs);
                portReserver->setPorts(manager->basePort, 4);
            }

            if (!strList.isEmpty()) {
                printf("Host addresses:\n");
                for (const QString &s : strList)
                    printf("  %s\n", qPrintable(s));
            }

            ice = new XMPP::Ice176(q);

            iceStarted = false;
            //            iceA_status.channelsReady.resize(2);
            //            iceA_status.channelsReady[0] = false;
            //            iceA_status.channelsReady[1] = false;

            q->connect(ice, &XMPP::Ice176::started, [this]() {
                for (auto &c : channels) {
                    ice->flagComponentAsLowOverhead((c->hints() & Connection::AvoidRelays) ? 0 : 1);
                }
                iceStarted = true;
            });
            q->connect(ice, &XMPP::Ice176::error, [this](XMPP::Ice176::Error err) {
                q->_lastReason = Reason(Reason::Condition::FailedTransport, "ICE failed");
                q->setState(State::Finished);
                emit q->failed();
            });
            q->connect(ice, &XMPP::Ice176::localCandidatesReady,
                       [this](const QList<XMPP::Ice176::Candidate> &candidates) {
                           localCandidates = candidates;
                           if (q->_state >= State::ApprovedToSend)
                               emit q->updated();
                       });
            QObject::connect(
                ice, &XMPP::Ice176::componentReady, q,
                [this](int componentIdx) {
                    for (auto &c : channels) {
                        if (c->channelIndex == componentIdx) {
                            c->onConnected(ice);
                        }
                    }
                },
                Qt::QueuedConnection); // signal is not DOR-SS

            ice->setProxy(manager->stunProxy);
            if (portReserver)
                ice->setPortReserver(portReserver);

            // QList<XMPP::Ice176::LocalAddress> localAddrs;
            // XMPP::Ice176::LocalAddress addr;

            // FIXME: the following is not true, a local address is not
            //   required, for example if you use TURN with TCP only

            // a local address is required to use ice.  however, if
            //   we don't have a local address, we won't handle it as
            //   an error here.  instead, we'll start Ice176 anyway,
            //   which should immediately error back at us.
            /*if(manager->selfAddr.isNull())
            {
                printf("no self address to use.  this will fail.\n");
                return;
            }

            addr.addr = manager->selfAddr;
            localAddrs += addr;*/
            ice->setLocalAddresses(localAddrs);

            // if an external address is manually provided, then apply
            //   it only to the selfAddr.  FIXME: maybe we should apply
            //   it to all local addresses?
            if (!extAddr.isNull()) {
                QList<XMPP::Ice176::ExternalAddress> extAddrs;
                /*XMPP::Ice176::ExternalAddress eaddr;
                eaddr.base = addr;
                eaddr.addr = extAddr;
                extAddrs += eaddr;*/
                for (const XMPP::Ice176::LocalAddress &la : localAddrs) {
                    XMPP::Ice176::ExternalAddress ea;
                    ea.base = la;
                    ea.addr = extAddr;
                    extAddrs += ea;
                }
                ice->setExternalAddresses(extAddrs);
            }

            if (!stunBindAddr.isNull() && stunBindPort > 0)
                ice->setStunBindService(stunBindAddr, stunBindPort);
            if (!stunRelayUdpAddr.isNull() && !manager->stunRelayUdpUser.isEmpty())
                ice->setStunRelayUdpService(stunRelayUdpAddr, stunRelayUdpPort, manager->stunRelayUdpUser,
                                            manager->stunRelayUdpPass.toUtf8());
            if (!stunRelayTcpAddr.isNull() && !manager->stunRelayTcpUser.isEmpty())
                ice->setStunRelayTcpService(stunRelayTcpAddr, stunRelayTcpPort, manager->stunRelayTcpUser,
                                            manager->stunRelayTcpPass.toUtf8());

            // RTP+RTCP
            ice->setComponentCount(channels.count());

            ice->setLocalFeatures(Ice176::Trickle);

            auto mode = q->creator() == q->pad()->session()->role() ? XMPP::Ice176::Initiator : XMPP::Ice176::Responder;
            ice->start(mode);
        }
    };

    Transport::Transport(const TransportManagerPad::Ptr &pad, Origin creator) :
        XMPP::Jingle::Transport(pad, creator), d(new Private)
    {
        d->q = this;
        connect(_pad->manager(), &TransportManager::abortAllRequested, this, [this]() {
            d->aborted = true;
            _state     = State::Finished;
            emit failed();
        });
    }

    Transport::~Transport()
    {
        if (d) { }
    }

    void Transport::prepare()
    {
        qDebug("Prepare local offer");
        setState(State::ApprovedToSend);
        // auto md = static_cast<Manager *>(_pad.staticCast<Pad>()->manager())->d.data();
        /*
         if (_creator == _pad->session()->role()) { // I'm creator
             d->sid = _pad.staticCast<Pad>()->generateSid();
         }
         _pad.staticCast<Pad>()->registerSid(d->sid);
         d->directAddr = makeKey(d->sid, _pad.staticCast<Pad>()->session()->initiator(),
                                 _pad.staticCast<Pad>()->session()->responder());
         m->addKeyMapping(d->directAddr, this);
 */
        // auto scope = _pad.staticCast<Pad>()->discoScope();
        // d->disco   = scope->disco(); // FIXME store and handle signale. delete when not needed

        auto manager  = dynamic_cast<Manager *>(_pad->manager())->d.data();
        auto resolver = new Resolver();
        connect(resolver, &Resolver::finished, this, [this, resolver]() {
            d->extAddr          = resolver->extAddr;
            d->stunBindAddr     = resolver->stunBindAddr;
            d->stunRelayUdpAddr = resolver->stunRelayUdpAddr;
            d->stunRelayTcpAddr = resolver->stunRelayTcpAddr;

            printf("resolver finished\n");
            resolver->deleteLater();
            d->startIce();
        });
        d->resolver.start(manager->extHost, manager->stunBindHost, manager->stunRelayUdpHost,
                          manager->stunRelayTcpHost);

        // connect(d->disco, &TcpPortDiscoverer::portAvailable, this, [this]() { d->onLocalServerDiscovered(); });
        // d->onLocalServerDiscovered();

        emit updated();
    }

    // we got content acceptance from any side and now can connect
    void Transport::start()
    {
        qDebug("Starting connecting");
        setState(State::Connecting);
    }

    bool Transport::update(const QDomElement &transportEl)
    {
        try {
            if (d->handleIncomingCandidate(transportEl) || d->handleIncomingRemoteCandidate(transportEl)
                || d->handleIncomingGatheringComplete(transportEl)) {
                if (_state == State::Created && _creator != _pad->session()->role()) {
                    // initial incoming transport
                    setState(State::Pending);
                }
                if (_state == State::Pending && _creator == _pad->session()->role()) {
                    // initial acceptance by remote of the local transport
                    setState(State::Accepted);
                }
                return true;
            }
        } catch (std::runtime_error &e) {
            qWarning("Transport updated failed: %s", e.what());
            return false;
        }

        return true;
    }

    bool Transport::hasUpdates() const { return isValid() && d->pendingActions; }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate()
    {
        qDebug("taking outgoing update");
        OutgoingTransportInfoUpdate upd;
        if (!isValid()) {
            return upd;
        }

        auto makeUpdate = [&](QDomElement tel, std::function<void()> cb = std::function<void()>()) {
            d->waitingAck = true;
            return OutgoingTransportInfoUpdate { tel, [this, cb, trptr = QPointer<Transport>(d->q)](bool success) {
                                                    if (!success || !trptr)
                                                        return;
                                                    d->waitingAck = false;
                                                    if (cb)
                                                        cb();
                                                } };
        };

        auto doc = _pad.staticCast<Pad>()->session()->manager()->client()->doc();

        QDomElement tel = doc->createElementNS(NS, "transport");
        // tel.setAttribute(QStringLiteral("sid"), d->sid);

        // check where we make initial offer
        bool noPending = (d->localCandidates.isEmpty() && !d->proxyDiscoveryInProgress
                          && !(d->disco && d->disco->inProgressPortTypes()));
        bool initial   = _state == State::ApprovedToSend && !d->offerSent
            && ((!d->pendingActions && noPending) || d->pendingActions & Private::NewCandidate);

        if (initial) {
            d->offerSent = true;
        }

        if (d->pendingActions & Private::NewCandidate) {
            d->pendingActions &= ~Private::NewCandidate;
            QList<XMPP::Ice176::Candidate> candidatesToSend;
            for (auto &c : d->localCandidates) {
                //                if (c.state() != Candidate::New) {
                //                    continue;
                //                }
                //                if (c.type() == Candidate::Proxy) {
                //                    useProxy = true;
                //                }
                //                qDebug("sending local candidate: cid=%s", qPrintable(c.cid()));
                //                tel.appendChild(c.toXml(doc));
                //                candidatesToSend.append(c);
                //                c.setState(Candidate::Unacked);
            }
            if (!candidatesToSend.isEmpty()) {
                upd = makeUpdate(tel, [this, candidatesToSend, initial]() mutable {
                    if (initial) {
                        _state = _creator == _pad->session()->role() ? State::Pending : State::Accepted;
                    }
                    // d->checkAndFinishNegotiation();
                });
            } else {
                qWarning("Got NewCandidate pending action but no candidate to send");
            }
        } else if (d->pendingActions & Private::RemoteCandidate) {
            d->pendingActions &= ~Private::RemoteCandidate;
            // we should have the only remote candidate in Pending state.
            // all other has to be discarded by priority check
            for (auto &c : d->remoteCandidates) {
                //                if (c.state() != Candidate::Pending) {
                //                    continue;
                //                }
                //                qDebug("sending candidate-used: cid=%s", qPrintable(c.cid()));
                auto el = tel.appendChild(doc->createElement(QStringLiteral("remote-candidate"))).toElement();
                // el.setAttribute(QStringLiteral("cid"), c.cid());
                // c.setState(Candidate::Unacked);

                upd = makeUpdate(tel, [this, c]() mutable {
                    //                    if (c.state() == Candidate::Unacked) {
                    //                        c.setState(Candidate::Accepted);
                    //                        qDebug("ack: sending candidate-used: cid=%s", qPrintable(c.cid()));
                    //                        d->remoteUsedCandidate = c;
                    //                    }
                    // d->checkAndFinishNegotiation();
                });
                break;
            }
            if (std::get<0>(upd).isNull()) {
                qWarning("Got CandidateUsed pending action but no pending candidates");
            }
        } else if (d->pendingActions & Private::GatheringComplete) {
            d->pendingActions &= ~Private::GatheringComplete;
            qDebug("sending gathering-complete");
            // we are here because all remote are already in Discardd state
            tel.appendChild(doc->createElement(QStringLiteral("gathering-complete")));
            upd = makeUpdate(tel, [this]() mutable {
                // d->localReportedGatheringComplete = true;
                // d->checkAndFinishNegotiation();
            });
        } else {
            qDebug("sending empty transport-info");
            upd = makeUpdate(tel, [this, initial]() mutable {
                if (initial) {
                    _state = _creator == _pad->session()->role() ? State::Pending : State::Accepted;
                }
            });
        }

        return upd; // TODO
    }

    bool Transport::isValid() const { return d != nullptr; }

    TransportFeatures Transport::features() const
    {
        return TransportFeatures(TransportFeature::HardToConnect) | TransportFeature::Reliable | TransportFeature::Fast;
    }

    int Transport::maxSupportedChannels() const { return -1; };

    Connection::Ptr Transport::addChannel() const
    {
        if (_state >= State::ApprovedToSend) {
            qWarning("Adding channel after negotiation start is not yet supported");
            return Connection::Ptr();
        }
        int  channelIdx = 0;
        auto it         = d->channels.constBegin();
        while (it != d->channels.constEnd()) {
            if (it.key() != channelIdx)
                break;
            channelIdx++;
            ++it;
        }
        auto conn = QSharedPointer<Connection>::create(channelIdx);
        d->channels.insert(it, channelIdx, conn);

        return conn.staticCast<XMPP::Jingle::Connection>();
    }

    //----------------------------------------------------------------
    // Manager
    //----------------------------------------------------------------
    Manager::Manager(QObject *parent) : TransportManager(parent), d(new Private) { }

    Manager::~Manager()
    {
        if (d->jingleManager)
            d->jingleManager->unregisterTransport(NS);
    }

    TransportFeatures Manager::features() const
    {
        return TransportFeatures(TransportFeature::Reliable) | TransportFeature::NotReliable
            | TransportFeature::RealTime;
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { d->jingleManager = jm; }

    QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, Origin creator)
    {
        return QSharedPointer<Transport>::create(pad, creator).staticCast<XMPP::Jingle::Transport>();
    }

    TransportManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    void Manager::closeAll() { emit abortAllRequested(); }

    void Manager::setBasePort(int port) { d->basePort = port; }

    void Manager::setExternalAddress(const QString &host) { d->extHost = host; }

    void Manager::setSelfAddress(const QHostAddress &addr) { d->selfAddr = addr; }

    void Manager::setStunBindService(const QString &host, int port)
    {
        d->stunBindHost = host;
        d->stunBindPort = port;
    }

    void Manager::setStunRelayUdpService(const QString &host, int port, const QString &user, const QString &pass)
    {
        d->stunRelayUdpHost = host;
        d->stunRelayUdpPort = port;
        d->stunRelayUdpUser = user;
        d->stunRelayUdpPass = pass;
    }

    void Manager::setStunRelayTcpService(const QString &host, int port, const XMPP::AdvancedConnector::Proxy &proxy,
                                         const QString &user, const QString &pass)
    {
        d->stunRelayTcpHost = host;
        d->stunRelayTcpPort = port;
        d->stunRelayTcpUser = user;
        d->stunRelayTcpPass = pass;

        XMPP::TurnClient::Proxy tproxy;

        if (proxy.type() == XMPP::AdvancedConnector::Proxy::HttpConnect) {
            tproxy.setHttpConnect(proxy.host(), proxy.port());
            tproxy.setUserPass(proxy.user(), proxy.pass());
        } else if (proxy.type() == XMPP::AdvancedConnector::Proxy::Socks) {
            tproxy.setSocks(proxy.host(), proxy.port());
            tproxy.setUserPass(proxy.user(), proxy.pass());
        }

        d->stunProxy = tproxy;
    }

    // void Manager::addKeyMapping(const QString &key, Transport *transport) { d->key2transport.insert(key, transport);
    // }

    // void Manager::removeKeyMapping(const QString &key) { d->key2transport.remove(key); }

    //    QString Manager::generateSid(const Jid &remote)
    //    {
    //        auto servers =
    //        d->jingleManager->client()->tcpPortReserver()->scope(QString::fromLatin1("s5b"))->allServers(); QString
    //        sid; QPair<Jid, QString> key; QString             key1; QString             key2; auto servChecker =
    //        [&](const TcpPortServer::Ptr &s) {
    //            return s.staticCast<S5BServer>()->hasKey(key1) || s.staticCast<S5BServer>()->hasKey(key2);
    //        };

    //        do {
    //            sid  = QString("s5b_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
    //            key  = qMakePair(remote, sid);
    //            key1 = makeKey(sid, remote, d->jingleManager->client()->jid());
    //            key2 = makeKey(sid, d->jingleManager->client()->jid(), remote);
    //        } while (d->sids.contains(key) || std::find_if(servers.begin(), servers.end(), servChecker) !=
    //        servers.end()); return sid;
    //    }

    // void Manager::registerSid(const Jid &remote, const QString &sid) { d->sids.insert(qMakePair(remote, sid)); }

    // Jid Manager::userProxy() const { return d->proxy; }

    // void Manager::setUserProxy(const Jid &jid) { d->proxy = jid; }

    //----------------------------------------------------------------
    // Pad
    //----------------------------------------------------------------
    Pad::Pad(Manager *manager, Session *session) : _manager(manager), _session(session)
    {
        auto reserver = _session->manager()->client()->tcpPortReserver();
        _discoScope   = reserver->scope(QString::fromLatin1("s5b"));
    }

    QString Pad::ns() const { return NS; }

    Session *Pad::session() const { return _session; }

    TransportManager *Pad::manager() const { return _manager; }

} // namespace Ice
} // namespace Jingle
} // namespace XMPP

#include "jingle-ice.moc"
