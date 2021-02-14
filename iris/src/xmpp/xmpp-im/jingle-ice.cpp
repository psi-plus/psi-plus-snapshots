/*
 * jignle-ice.cpp - Jingle ICE transport
 * Copyright (C) 2020  Sergey Ilinykh
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

#include "irisnet/noncore/sctp/DepUsrSCTP.hpp" //Do not move to avoid warnings with MinGW

#include "jingle-ice.h"

#include "dtls.h"
#include "ice176.h"
#include "jingle-session.h"
#include "netnames.h"
#include "udpportreserver.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_xmlcommon.h"

#include <array>
#include <memory>

#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTimer>

namespace XMPP { namespace Jingle { namespace ICE {
    const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
    const QString NS_DTLS(QStringLiteral("urn:xmpp:jingle:apps:dtls:0"));
    const QString NS_SCTP(QStringLiteral("urn:xmpp:jingle:transports:dtls-sctp:1"));

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

    struct SctpKeeper {
        using Ptr = std::shared_ptr<SctpKeeper>;

        static std::weak_ptr<SctpKeeper> instance;
        SctpKeeper() { DepUsrSCTP::ClassInit(); }
        ~SctpKeeper() { DepUsrSCTP::ClassDestroy(); }
        static Ptr use()
        {
            auto i = instance.lock();
            if (!i) {
                i        = std::make_shared<SctpKeeper>();
                instance = i;
            }
            return i;
        }
    };
    std::weak_ptr<SctpKeeper> SctpKeeper::instance;

    enum class SctpProtocol { None, WebRTCDataChannel };

    struct SctpElement {
        SctpProtocol protocol = SctpProtocol::None;
        uint16_t     number   = 0;

        QDomElement toXml(QDomDocument *doc)
        {
            QDomElement ret;
            if (protocol == SctpProtocol::None)
                return ret;
            ret = doc->createElementNS(NS_SCTP, QLatin1String("sctpmap"));
            ret.setAttribute(QLatin1String("protocol"), QLatin1String("webrtc-datachannel"));
            ret.setAttribute(QLatin1String("number"), number);
            return ret;
        }

        bool parse(const QDomElement &el)
        {
            if (el.namespaceURI() != NS_SCTP) {
                return false;
            }
            auto p = el.attribute(QLatin1String("protocol"));
            protocol
                = (p == QLatin1String("webrtc-datachannel")) ? SctpProtocol::WebRTCDataChannel : SctpProtocol::None;
            number = el.attribute(QLatin1String("number")).toInt();
            return protocol != SctpProtocol::None && number > 0;
        }
    };

    static std::array<const char *, 4> fpRoles { { "active", "passive", "actpass", "holdconn" } };
    struct FingerPrint {
        enum Setup { Active, Passive, ActPass, HoldConn };

        Hash  hash;
        Setup setup = Setup(-1);

        FingerPrint(const QDomElement &el)
        {
            auto ht = el.attribute(QLatin1String("hash"));
            hash    = QStringRef(&ht);
            hash.setData(QByteArray::fromHex(el.text().toLatin1()));
            auto setupIt = std::find(fpRoles.begin(), fpRoles.end(),
                                     el.attribute(QLatin1String("setup")).toLatin1().constData());
            setup        = Setup(setupIt == fpRoles.end() ? -1 : std::distance(fpRoles.begin(), setupIt));
        }
        FingerPrint(const Hash &hash, Setup setup) : hash(hash), setup(setup) { }

        inline bool isValid() const
        {
            return hash.isValid() && !hash.data().isEmpty() && setup >= Active && setup <= HoldConn;
        }
        QDomElement toXml(QDomDocument *doc) const
        {
            auto binToHex = [](const QByteArray &in) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
                return in.toHex(':');
#else
                QByteArray out  = in.toHex();
                int        size = out.size();
                for (int k = 2; k < size; k += 3, ++size) {
                    out.insert(k, ':');
                }
                return out;
#endif
            };
            auto fingerprint = XMLHelper::textTagNS(doc, NS_DTLS, QLatin1String("fingerprint"), binToHex(hash.data()));
            fingerprint.setAttribute(QLatin1String("hash"), hash.stringType());
            fingerprint.setAttribute(QLatin1String("setup"), QLatin1String(fpRoles[setup]));
            return fingerprint;
        }
    };

    class Resolver : public QObject {
        Q_OBJECT
        using QObject::QObject;

        int                   counter;
        std::function<void()> callback;

        void onOneFinished()
        {
            if (!--counter) {
                callback();
                deleteLater();
            }
        }

    public:
        using ResolveList = std::list<std::pair<QString, std::reference_wrapper<QHostAddress>>>;

        static void resolve(QObject *parent, ResolveList list, std::function<void()> &&callback)
        {
            auto resolver      = new Resolver(parent);
            resolver->counter  = list.size();
            resolver->callback = callback;
            for (auto &item : list) {
                auto *dns = new NameResolver(parent);

                connect(dns, &NameResolver::resultsReady, resolver,
                        [result = item.second, resolver](const QList<XMPP::NameRecord> &records) {
                            result.get() = records.first().address();
                            resolver->onOneFinished();
                        });
                connect(dns, &NameResolver::error, resolver,
                        [resolver](XMPP::NameResolver::Error) { resolver->onOneFinished(); });

                dns->start(item.first.toLatin1());
            }
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

        void start(XMPP::UdpPortReserver *_portReserver, const QList<Ice176 *> iceList)
        {
            if (_portReserver) {
                portReserver = _portReserver;
                portReserver->setParent(this);
            }
            left = iceList;

            for (Ice176 *ice : left) {
                ice->setParent(this);

                // TODO: error() also?
                connect(ice, &Ice176::stopped, this, &IceStopper::ice_stopped);
                connect(ice, &Ice176::error, this, &IceStopper::ice_error);
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
        SctpKeeper::Ptr         sctpInstance;

        // FIMME it's reuiqred to split transports by direction otherwise we gonna hit conflicts.
        // jid,transport-sid -> transport mapping
        //        QSet<QPair<Jid, QString>>   sids;
        //        QHash<QString, Transport *> key2transport;
        //        Jid                         proxy;
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

    static QDomElement candidateToElement(QDomDocument *doc, const XMPP::Ice176::Candidate &c)
    {
        QDomElement e = doc->createElement("candidate");
        e.setAttribute("component", QString::number(c.component));
        e.setAttribute("foundation", c.foundation);
        e.setAttribute("generation", QString::number(c.generation));
        if (!c.id.isEmpty())
            e.setAttribute("id", c.id);
        e.setAttribute("ip", c.ip.toString());
        if (c.network != -1)
            e.setAttribute("network", QString::number(c.network));
        else // weird?
            e.setAttribute("network", QString::number(0));
        e.setAttribute("port", QString::number(c.port));
        e.setAttribute("priority", QString::number(c.priority));
        e.setAttribute("protocol", c.protocol);
        if (!c.rel_addr.isNull())
            e.setAttribute("rel-addr", c.rel_addr.toString());
        if (c.rel_port != -1)
            e.setAttribute("rel-port", QString::number(c.rel_port));
        // TODO: remove this?
        // if(!c.rem_addr.isNull())
        //    e.setAttribute("rem-addr", c.rem_addr.toString());
        // if(c.rem_port != -1)
        //    e.setAttribute("rem-port", QString::number(c.rem_port));
        e.setAttribute("type", c.type);
        return e;
    }

    class Connection : public XMPP::Jingle::Connection {
        Q_OBJECT
    public:
        QList<NetworkDatagram> datagrams;
        void *                 client;
        int                    component; // starting from 0
        TransportFeatures      features;

        Connection(int component, TransportFeatures features) : component(component), features(features)
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

        void onConnected(Ice176 *ice)
        {
            qDebug("ice channel connected!");
            emit connected();
        }

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
        bool                           initialOfferReady               = false;
        bool                           remoteReportedGatheringComplete = false;
        bool                           iceStarted                      = false;
        quint16                        pendingActions                  = 0;
        int                            proxiesInDiscoCount             = 0;
        int                            components                      = 0;
        QList<XMPP::Ice176::Candidate> pendingLocalCandidates; // cid to candidate mapping
        QList<XMPP::Ice176::Candidate> remoteCandidates;
        QString                        remoteUfrag;
        QString                        remotePassword;
        SctpElement                    sctp;

        // QString            sid;
        // Transport::Mode    mode = Transport::Tcp;
        // QTimer             probingTimer;
        // QTimer             negotiationFinishTimer;
        // QElapsedTimer      lastConnectionStart;
        // size_t             blockSize    = 8192;
        TcpPortDiscoverer *disco        = nullptr;
        UdpPortReserver *  portReserver = nullptr;
        Resolver           resolver;
        XMPP::Ice176 *     ice  = nullptr;
        XMPP::Dtls *       dtls = nullptr;

        QHostAddress extAddr;
        QHostAddress stunBindAddr, stunRelayUdpAddr, stunRelayTcpAddr;
        int          stunBindPort;
        int          stunRelayUdpPort;
        int          stunRelayTcpPort;

        QList<QSharedPointer<Connection>> channels;

        // udp stuff
        bool         udpInitialized;
        quint16      udpPort;
        QHostAddress udpAddress;

        inline Jid remoteJid() const { return q->_pad->session()->peer(); }

        void flushRemoteCandidates()
        {
            if (!ice || q->_state < State::ApprovedToSend || q->_state == State::Finished)
                return;
            ice->setPeerUfrag(remoteUfrag);
            ice->setPeerPassword(remotePassword);
            if (remoteCandidates.isEmpty())
                return;
            ice->addRemoteCandidates(remoteCandidates);
            remoteCandidates.clear();

            std::sort(remoteCandidates.begin(), remoteCandidates.end(),
                      [](const auto &a, const auto &b) { return a.component < b.component; });
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
                if (ic.component > components) {
                    if (q->_state >= State::ApprovedToSend) {
                        throw std::runtime_error("too late to add components");
                    }
                    components = ic.component;
                }
                // qDebug("new remote candidate: %s", qPrintable(c.toString()));
                remoteCandidates.append(ic); // TODO check for collisions!
                candidatesAdded++;
            }
            if (!candidatesAdded) {
                return false;
            }
            if (q->isRemote() && channels.empty()) {
                /*
                 * seems like initial offer
                 *
                 * The channels split topic is somewhat complicated.
                 * We can talk about multiplexed rtp/rtcp channels, about sctp channels or maybe plain ICE components.
                 * In general logic is as following:
                 *  - no datachannels - amount of channels = amount of components
                 *  - with datachannels - one channel for datachannel on component 1. one channel per component with
                 *                        filtered out datachannel
                 */
                TransportFeatures features(TransportFeature::HighProbableConnect | TransportFeature::Fast
                                           | TransportFeature::MessageOriented);
                if (sctp.protocol != SctpProtocol::None) {
                    channels.append(QSharedPointer<Connection>::create(0, features | TransportFeature::DataOriented));
                }
                for (int i = 0; i < components; i++) {
                    channels.append(QSharedPointer<Connection>::create(i, features));
                }
            }
            if (ice) {
                QTimer::singleShot(0, q, [this]() { flushRemoteCandidates(); });
            }
            return true;
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
                                auto cUsed = pendingLocalCandidates.value(el.attribute(QStringLiteral("cid")));
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
                for (auto &c : pendingLocalCandidates) {
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
                qDebug("STUN service: %s;%d", qPrintable(stunBindAddr.toString()), stunBindPort);
            if (!stunRelayUdpAddr.isNull() && stunRelayUdpPort > 0 && !manager->stunRelayUdpUser.isEmpty())
                qDebug("TURN w/ UDP service: %s;%d", qPrintable(stunRelayUdpAddr.toString()), stunRelayUdpPort);
            if (!stunRelayTcpAddr.isNull() && stunRelayTcpPort > 0 && !manager->stunRelayTcpUser.isEmpty())
                qDebug("TURN w/ TCP service: %s;%d", qPrintable(stunRelayTcpAddr.toString()), stunRelayTcpPort);

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

            ice = new Ice176(q);

            iceStarted = false;
            //            iceA_status.channelsReady.resize(2);
            //            iceA_status.channelsReady[0] = false;
            //            iceA_status.channelsReady[1] = false;

            q->connect(ice, &XMPP::Ice176::started, [this]() {
                QSet<int> lowOverhead;
                for (auto &c : channels) {
                    if (!(c->hints() & Connection::AvoidRelays)) {
                        lowOverhead.insert(c->component);
                    }
                }
                for (auto componentIndex : lowOverhead) {
                    ice->flagComponentAsLowOverhead(componentIndex);
                }
                iceStarted = true;
            });
            q->connect(ice, &XMPP::Ice176::error, [this](XMPP::Ice176::Error err) {
                q->_lastReason = Reason(Reason::Condition::FailedTransport, QString("ICE failed: %1").arg(err));
                q->setState(State::Finished);
                emit q->failed();
            });
            q->connect(ice, &XMPP::Ice176::localCandidatesReady,
                       [this](const QList<XMPP::Ice176::Candidate> &candidates) {
                           pendingActions |= NewCandidate;
                           pendingLocalCandidates += candidates;
                           emit q->updated();
                       });
            q->connect(ice, &XMPP::Ice176::localGatheringComplete, [this]() {
                pendingActions |= GatheringComplete;
                emit q->updated();
            });
            QObject::connect(
                ice, &XMPP::Ice176::readyToSendMedia, q,
                [this]() {
                    for (auto &c : channels) {
                        c->onConnected(ice);
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
            ice->setComponentCount(components);

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

    Transport::~Transport() { qDebug("jingle-ice: destroyed"); }

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

        auto manager = dynamic_cast<Manager *>(_pad->manager())->d.data();

        Resolver::resolve(this,
                          { { manager->extHost, std::ref(d->extAddr) },
                            { manager->stunBindHost, std::ref(d->stunBindAddr) },
                            { manager->stunRelayUdpHost, std::ref(d->stunRelayUdpAddr) },
                            { manager->stunRelayTcpHost, std::ref(d->stunRelayTcpAddr) } },
                          [this]() {
                              printf("resolver finished\n");
                              d->startIce();
                          });

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
            qWarning("Transport update failed: %s", e.what());
            return false;
        }

        return true;
    }

    bool Transport::hasUpdates() const { return isValid() && d->pendingActions; }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate([[maybe_unused]] bool ensureTransportElement = false)
    {
        OutgoingTransportInfoUpdate upd;
        if (!hasUpdates() || _state < State::ApprovedToSend || (isRemote() && _state == State::Pending)) {
            return upd;
        }

        qDebug("jingle-ice: taking outgoing update");
        auto        doc = _pad.staticCast<Pad>()->session()->manager()->client()->doc();
        QDomElement tel = doc->createElementNS(NS, "transport");
        tel.setAttribute(QLatin1String("pwd"), d->ice->localPassword());
        tel.setAttribute(QLatin1String("ufrag"), d->ice->localUfrag());
        if (d->dtls) {
            tel.appendChild(
                FingerPrint(d->dtls->fingerprint(),
                            _pad->session()->role() == Origin::Initiator ? FingerPrint::ActPass : FingerPrint::Active)
                    .toXml(doc));
        }

        for (auto const &cand : d->pendingLocalCandidates) {
            tel.appendChild(candidateToElement(doc, cand));
        }
        d->pendingLocalCandidates.clear();
        d->pendingActions &= ~Private::NewCandidate;

        if (d->pendingActions & Private::GatheringComplete) {
            tel.appendChild(doc->createElement(QLatin1String("gathering-complete")));
            d->pendingActions &= ~Private::GatheringComplete;
        }

        if (d->pendingActions & Private::RemoteCandidate) {
            for (auto const &c : d->ice->selectedCandidates()) {
                auto rc = doc->createElement(QLatin1String("remote-candidate"));
                rc.setAttribute(QLatin1String("component"), c.componentId);
                rc.setAttribute(QLatin1String("ip"), c.ip.toString());
                rc.setAttribute(QLatin1String("port"), c.port);
                tel.appendChild(rc);
            }
            d->pendingActions &= ~Private::RemoteCandidate;
        }

        d->waitingAck = true;
        return OutgoingTransportInfoUpdate { tel, [this, trptr = QPointer<Transport>(d->q)](bool success) {
                                                if (!success || !trptr)
                                                    return;
                                                d->waitingAck = false;
                                            } };
    }

    bool Transport::isValid() const { return d != nullptr; }

    TransportFeatures Transport::features() const { return _pad->manager()->features(); }

    int Transport::maxSupportedChannelsPerComponent(TransportFeatures features) const
    {
        return features & TransportFeature::DataOriented ? 65536 : 1;
    };

    int Transport::addComponent()
    {
        if (_state >= State::ApprovedToSend) {
            qWarning("adding component after ICE started is not supported");
            return 0;
        }
        d->components++;
        return d->components;
    }

    // adding ice components (for rtp, rtcp, datachannel etc)
    // but those are rather abstract channels and it's up to ice manager in TransportPad to decide
    Connection::Ptr Transport::addChannel(TransportFeatures features, int component) const
    {
        // features define type of channel. reliable channels infer sctp
        // if time-oriented - likely rtp
        // if data-oriented - likely sctp

        if (!((isLocal() && _state == State::Created) || (isRemote() && _state == State::Pending))) {
            qWarning("Adding channel after negotiation start is not yet supported");
            return Connection::Ptr();
        }

        if (component >= d->components) {
            d->components = component + 1;
        }
        // find a gap in the list of channel indexes or just take last one
        auto conn
            = QSharedPointer<Connection>::create(features & TransportFeature::DataOriented ? 0 : component, features);
        d->channels.append(conn);

        return conn.staticCast<XMPP::Jingle::Connection>();
    }

    QList<XMPP::Jingle::Connection::Ptr> Transport::channels() const
    {
        QList<XMPP::Jingle::Connection::Ptr> ret;
        for (auto &c : d->channels) {
            ret.append(c.staticCast<XMPP::Jingle::Connection>());
        }
        return ret;
    }

    //----------------------------------------------------------------
    // Manager
    //----------------------------------------------------------------
    Manager::Manager(QObject *parent) : TransportManager(parent), d(new Private)
    {
        d->sctpInstance = SctpKeeper::use();
    }

    Manager::~Manager()
    {
        if (d->jingleManager) {
            d->jingleManager->unregisterTransport(NS);
        }
    }

    TransportFeatures Manager::features() const
    {
        return TransportFeature::HighProbableConnect | TransportFeature::Reliable | TransportFeature::Unreliable
            | TransportFeature::MessageOriented | TransportFeature::DataOriented | TransportFeature::LiveOriented;
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { d->jingleManager = jm; }

    QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, Origin creator)
    {
        return QSharedPointer<Transport>::create(pad, creator).staticCast<XMPP::Jingle::Transport>();
    }

    TransportManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    void Manager::closeAll() { emit abortAllRequested(); }

    QStringList Manager::discoFeatures() const { return { NS, NS_DTLS }; }

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

    //----------------------------------------------------------------
    // Pad
    //----------------------------------------------------------------
    Pad::Pad(Manager *manager, Session *session) : _manager(manager), _session(session)
    {
        auto reserver = _session->manager()->client()->tcpPortReserver();
        _discoScope   = reserver->scope(QString::fromLatin1("ice"));
    }

    QString Pad::ns() const { return NS; }

    Session *Pad::session() const { return _session; }

    TransportManager *Pad::manager() const { return _manager; }

    void Pad::onLocalAccepted()
    {
        if (!_session->isGroupingAllowed() && _session->role() != Origin::Initiator)
            return;
        QStringList bundle;
        for (auto app : session()->contentList()) {
            auto transport = app->transport();
            if (transport && transport.dynamicCast<Transport>()) {
                // do grouping stuff
                bundle.append(app->contentName());
            }
        }
        if (bundle.size() > 1)
            _session->setGrouping(QLatin1String("BUNDLE"), bundle);
    }

} // namespace Ice
} // namespace Jingle
} // namespace XMPP

#include "jingle-ice.moc"
