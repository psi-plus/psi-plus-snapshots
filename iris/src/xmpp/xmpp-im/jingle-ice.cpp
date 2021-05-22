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

#ifdef JINGLE_SCTP
#include "jingle-sctp.h" //Do not move to avoid warnings with MinGW
#endif

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

template <class T> constexpr std::add_const_t<T> &as_const(T &t) noexcept { return t; }

namespace XMPP { namespace Jingle { namespace ICE {
    const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
    const QString NS_DTLS(QStringLiteral("urn:xmpp:jingle:apps:dtls:0"));

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

        // TODO tcptype

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
        // e.setAttribute("ip", "192.168.0.99");
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

    QDomElement remoteCandidateToElement(QDomDocument *doc, const Ice176::SelectedCandidate &c)
    {
        auto e = doc->createElement("remote-candidate");
        e.setAttribute(QLatin1String("component"), c.componentId);
        e.setAttribute(QLatin1String("ip"), c.ip.toString());
        e.setAttribute(QLatin1String("port"), c.port);
        return e;
    }

    Ice176::SelectedCandidate elementToRemoteCandidate(const QDomElement &el, bool *ok)
    {
        Ice176::SelectedCandidate c;
        quint16                   tmp;
        tmp = el.attribute(QLatin1String("component")).toUInt(ok);
        *ok = *ok && tmp < 256;
        if (!*ok)
            return c;
        c.componentId = tmp;
        c.ip          = QHostAddress(el.attribute(QLatin1String("ip")));
        c.port        = el.attribute(QLatin1String("port")).toUShort(ok);
        *ok           = *ok && !c.ip.isNull();
        return c;
    }

    struct Element {
        QString           pwd;
        QString           ufrag;
        Dtls::FingerPrint fingerprint;
#ifdef JINGLE_SCTP
        SCTP::MapElement            sctpMap;
        QList<SCTP::ChannelElement> sctpChannels;
#endif
        QList<Ice176::Candidate>         candidates;
        QList<Ice176::SelectedCandidate> remoteCandidates;
        bool                             gatheringComplete = false;

        void cleanupICE()
        {
            candidates.clear();
            remoteCandidates.clear();
            gatheringComplete = false;
        }

        QDomElement toXml(QDomDocument *doc) const
        {
            QDomElement tel = doc->createElementNS(NS, "transport");
            if (!pwd.isEmpty())
                tel.setAttribute(QLatin1String("pwd"), pwd);
            if (!ufrag.isEmpty())
                tel.setAttribute(QLatin1String("ufrag"), ufrag);
            if (fingerprint.isValid())
                tel.appendChild(fingerprint.toXml(doc));
#ifdef JINGLE_SCTP
            if (sctpMap.isValid())
                tel.appendChild(sctpMap.toXml(doc));
            for (auto const &c : sctpChannels)
                tel.appendChild(c.toXml(doc));
#endif
            for (auto const &c : candidates)
                tel.appendChild(candidateToElement(doc, c));
            for (auto const &c : remoteCandidates) {
                tel.appendChild(remoteCandidateToElement(doc, c));
            }
            if (gatheringComplete)
                tel.appendChild(doc->createElement(QLatin1String("gathering-complete")));

            return tel;
        }

        void parse(const QDomElement &el)
        {
            ufrag             = el.attribute(QLatin1String("ufrag"));
            pwd               = el.attribute(QLatin1String("pwd"));
            auto e            = el.firstChildElement(QLatin1String("gathering-complete"));
            gatheringComplete = !e.isNull();

            e = el.firstChildElement(QLatin1String("fingerprint"));
            if (!e.isNull() && !fingerprint.parse(e))
                throw std::runtime_error("invalid fingerprint");

            if (fingerprint.isValid() && !Dtls::isSupported())
                qWarning("Remote requested DTLS but it's not supported by used crypto libraries.");
#ifdef JINGLE_SCTP
            e = el.firstChildElement(QLatin1String("sctpmap"));
            if (!e.isNull() && !sctpMap.parse(e))
                throw std::runtime_error("invalid sctpmap");

            QString chTag(QStringLiteral("channel"));
            for (e = el.firstChildElement(chTag); !e.isNull(); e = e.nextSiblingElement(chTag)) {
                SCTP::ChannelElement channel;
                if (!channel.parse(e))
                    throw std::runtime_error("invalid sctp channel");
                ;
                sctpChannels.append(channel);
            }
#endif
            QString candTag(QStringLiteral("candidate"));
            for (e = el.firstChildElement(candTag); !e.isNull(); e = e.nextSiblingElement(candTag)) {
                auto c = elementToCandidate(e);
                if (!c.component || c.type.isEmpty())
                    throw std::runtime_error("invalid candidate");
                ;
                candidates.append(c);
            }
            if (!candidates.isEmpty()) {
                if (ufrag.isEmpty() || pwd.isEmpty())
                    throw std::runtime_error("user fragment or password can't be empty");
            }

            QString rcTag(QStringLiteral("remote-candidate"));
            for (e = el.firstChildElement(rcTag); !e.isNull(); e = e.nextSiblingElement(rcTag)) {
                bool                      ok;
                Ice176::SelectedCandidate rc = elementToRemoteCandidate(e, &ok);
                if (!ok)
                    throw std::runtime_error("invalid remote candidate");
                ;
                remoteCandidates.append(rc);
            }
        }
    };

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
            quint8  a0 = quint8(v4 >> 24);
            quint8  a1 = quint8((v4 >> 16) & 0xff);
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

        // FIMME it's reuiqred to split transports by direction otherwise we gonna hit conflicts.
        // jid,transport-sid -> transport mapping
        //        QSet<QPair<Jid, QString>>   sids;
        //        QHash<QString, Transport *> key2transport;
        //        Jid                         proxy;
    };

    class RawConnection : public XMPP::Jingle::Connection {
        Q_OBJECT
    public:
        using Ptr = QSharedPointer<RawConnection>;

        enum DisconnectReason { None, DtlsClosed };

        QList<NetworkDatagram> datagrams;
        DisconnectReason       disconnectReason = None;
        quint8                 componentIndex   = 0;

        RawConnection(quint8 componentIndex) : componentIndex(componentIndex) {};

        int component() const override { return componentIndex; }

        TransportFeatures features() const override
        {
            return TransportFeature::Fast | TransportFeature::MessageOriented | TransportFeature::HighProbableConnect
                | TransportFeature::Unreliable;
        }

        bool hasPendingDatagrams() const override { return datagrams.size() > 0; }

        NetworkDatagram readDatagram(qint64 maxSize = -1) override
        {
            Q_UNUSED(maxSize) // TODO or not?
            return datagrams.size() ? datagrams.takeFirst() : NetworkDatagram();
        }

        qint64 bytesAvailable() const override { return 0; }

        qint64 bytesToWrite() const override { return 0; /*client->bytesToWrite();*/ }

        void close() override { XMPP::Jingle::Connection::close(); }

    private:
        friend class Transport;

        void onConnected()
        {
            qDebug("jingle-ice: channel connected!");
            emit connected();
        }

        void onError(QAbstractSocket::SocketError error) { qDebug("jingle-ice: channel failed: %d", error); }

        void onDisconnected(DisconnectReason reason)
        {
            if (!isOpen())
                return;
            disconnectReason = reason;
            setOpenMode(QIODevice::ReadOnly);
            emit disconnected();
        }

        void enqueueIncomingUDP(const QByteArray &data)
        {
            datagrams.append(NetworkDatagram { data });
            emit readyRead();
        }
    };

    struct Component {
        int   componentIndex  = 0;
        bool  initialized     = false;
        bool  lowOverhead     = false;
        bool  needDatachannel = false;
        Dtls *dtls            = nullptr;
#ifdef JINGLE_SCTP
        SCTP::Association *sctp = nullptr;
#endif
        QSharedPointer<RawConnection> rawConnection;
        // QHash<quint16, Connection::Ptr> dataChannels;
    };

    class Transport::Private {
    public:
        enum PendingActions {
            NewCandidate       = 1,
            RemoteCandidate    = 2,
            GatheringComplete  = 4,
            NewFingerprint     = 8,
            NewSctpAssociation = 16
        };

        Transport *                    q                         = nullptr;
        bool                           offerSent                 = false;
        bool                           aborted                   = false;
        bool                           initialOfferReady         = false;
        bool                           remoteAcceptedFingerprint = false;
        quint16                        pendingActions            = 0;
        int                            proxiesInDiscoCount       = 0;
        QVector<Component>             components;
        QList<XMPP::Ice176::Candidate> pendingLocalCandidates; // cid to candidate mapping
        QScopedPointer<Element>        remoteState;

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

        Dtls::Setup localDtlsRole  = Dtls::ActPass;
        Dtls::Setup remoteDtlsRole = Dtls::ActPass;
#ifdef JINGLE_SCTP
        SCTP::MapElement sctp;
#endif

        QHostAddress extAddr;
        QHostAddress stunBindAddr, stunRelayUdpAddr, stunRelayTcpAddr;
        int          stunBindPort;
        int          stunRelayUdpPort;
        int          stunRelayTcpPort;

        // QList<QSharedPointer<Connection>> channels;

        // udp stuff
        bool         udpInitialized;
        quint16      udpPort;
        QHostAddress udpAddress;

        ~Private()
        {
            if (ice) {
                ice->disconnect(q);
                auto stopper = new IceStopper;
                stopper->start(portReserver, QList<Ice176 *>() << ice);
            }
        }

        inline Jid remoteJid() const { return q->_pad->session()->peer(); }

        Component &addComponent()
        {
            components.append(Component {});
            auto &c          = components.last();
            c.componentIndex = components.size() - 1;
            return c;
        }

        void setupDtls(int componentIndex)
        {
            qDebug("Setup DTLS");
            Q_ASSERT(componentIndex < components.length());
            if (components[componentIndex].dtls)
                return;
            components[componentIndex].dtls
                = new Dtls(q, q->pad()->session()->me().full(), q->pad()->session()->peer().full());

            auto dtls = components[componentIndex].dtls;
            if (q->isLocal()) {
                dtls->initOutgoing();
            } else {
                dtls->setRemoteFingerprint(remoteState->fingerprint);
                dtls->acceptIncoming();
            }

            if (componentIndex == 0) { // for other components it's the same but we don't need multiple fingerprints
                dtls->connect(
                    dtls, &Dtls::needRestart, q,
                    [this, componentIndex]() {
                        pendingActions |= NewFingerprint;
                        remoteAcceptedFingerprint = false;
                        emit q->updated();
                    },
                    Qt::QueuedConnection);
                pendingActions |= NewFingerprint;
            }
            dtls->connect(dtls, &Dtls::readyRead, q, [this, componentIndex]() {
                auto &component = components[componentIndex];
                auto  d         = component.dtls->readDatagram();
#ifdef JINGLE_SCTP
                if (component.sctp) {
                    component.sctp->writeIncoming(d);
                }
#endif
            });
            dtls->connect(dtls, &Dtls::readyReadOutgoing, q, [this, componentIndex]() {
                ice->writeDatagram(componentIndex, components[componentIndex].dtls->readOutgoingDatagram());
            });
            dtls->connect(dtls, &Dtls::connected, q, [this, componentIndex, dtls]() {
                auto &c = components[componentIndex];
#ifdef JINGLE_SCTP
                if (c.sctp) {
                    // see rfc8864 (6.1) and rfc8832 (6)
                    c.sctp->setIdSelector(dtls->localFingerprint().setup == Dtls::Active ? SCTP::IdSelector::Even
                                                                                         : SCTP::IdSelector::Odd);
                    c.sctp->onTransportConnected();
                }
#endif
                if (c.rawConnection)
                    c.rawConnection->onConnected();
            });
            dtls->connect(dtls, &Dtls::errorOccurred, q, [this, componentIndex](QAbstractSocket::SocketError error) {
                qDebug("dtls failed for component %d", componentIndex);
                auto &c = components[componentIndex];
#ifdef JINGLE_SCTP
                if (c.sctp)
                    c.sctp->onTransportError(error);
#endif
                if (c.rawConnection)
                    c.rawConnection->onError(error);
            });
            dtls->connect(dtls, &Dtls::closed, q, [this, componentIndex]() {
                qDebug("dtls closed for component %d", componentIndex);
                auto &c = components[componentIndex];
                if (c.rawConnection)
                    c.rawConnection->onDisconnected(RawConnection::DtlsClosed);
#ifdef JINGLE_SCTP
                if (c.sctp)
                    c.sctp->onTransportClosed();
#endif
            });
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

            auto listenAddrs = Ice176::availableNetworkAddresses();

            QList<XMPP::Ice176::LocalAddress> localAddrs;

            QStringList strList;
            for (const QHostAddress &h : as_const(listenAddrs)) {
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
                for (const QString &s : as_const(strList))
                    printf("  %s\n", qPrintable(s));
            }

            ice = new Ice176(q);

            q->connect(ice, &XMPP::Ice176::started, q, [this]() {
                for (auto const &c : as_const(components)) {
                    if (c.lowOverhead)
                        ice->flagComponentAsLowOverhead(c.componentIndex);
                }
            });
            q->connect(ice, &XMPP::Ice176::error, q, [this](XMPP::Ice176::Error err) {
                q->onFinish(Reason::Condition::ConnectivityError, QString("ICE failed: %1").arg(err));
            });
            q->connect(ice, &XMPP::Ice176::localCandidatesReady, q,
                       [this](const QList<XMPP::Ice176::Candidate> &candidates) {
                           pendingActions |= NewCandidate;
                           pendingLocalCandidates += candidates;
                           qDebug("discovered %d local candidates", candidates.size());
                           for (auto const &c : candidates) {
                               qDebug(" - %s:%d", qPrintable(c.ip.toString()), c.port);
                           }
                           emit q->updated();
                       });
            q->connect(ice, &XMPP::Ice176::localGatheringComplete, q, [this]() {
                pendingActions |= GatheringComplete;
                emit q->updated();
            });
            q->connect(
                ice, &XMPP::Ice176::readyToSendMedia, q,
                [this]() {
                    qDebug("ICE reported ready to send media!");
                    if (!components[0].dtls) // if empty
                        notifyRawConnected();
                    else if (remoteAcceptedFingerprint)
                        for (const auto &c : as_const(components))
                            c.dtls->onRemoteAcceptedFingerprint();
                },
                Qt::QueuedConnection); // signal is not DOR-SS
            q->connect(ice, &Ice176::readyRead, q, [this](int componentIndex) {
                qDebug("ICE readyRead");
                auto  buf       = ice->readDatagram(componentIndex);
                auto &component = components[componentIndex];
                if (component.dtls) {
                    component.dtls->writeIncomingDatagram(buf);
                } else if (component.rawConnection) {
                    component.rawConnection->enqueueIncomingUDP(buf);
                }
            });

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
                for (const XMPP::Ice176::LocalAddress &la : as_const(localAddrs)) {
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
            ice->setComponentCount(components.count());

            ice->setLocalFeatures(Ice176::Trickle);

            setupRemoteICE(*remoteState);
            remoteState->cleanupICE();

            auto mode = q->creator() == q->pad()->session()->role() ? XMPP::Ice176::Initiator : XMPP::Ice176::Responder;
            ice->start(mode);
        }

        void setupRemoteICE(const Element &e)
        {
            Q_ASSERT(ice != nullptr);
            if (!e.candidates.isEmpty()) {
                ice->setRemoteCredentials(e.ufrag, e.pwd);
                ice->addRemoteCandidates(e.candidates);
            }
            if (e.gatheringComplete) {
                ice->setRemoteGatheringComplete();
            }
            if (e.remoteCandidates.size()) {
                ice->setRemoteSelectedCandidadates(e.remoteCandidates);
            }
        }

        void handleRemoteUpdate(const Element &e)
        {
            if (q->state() == State::Finished)
                return;

            if (ice) {
                setupRemoteICE(e);
            } else {
                if (!e.candidates.isEmpty() || !e.ufrag.isEmpty()) {
                    remoteState->ufrag = e.ufrag;
                    remoteState->pwd   = e.pwd;
                }

                if (e.gatheringComplete) {
                    remoteState->gatheringComplete = true;
                }
                remoteState->candidates += e.candidates;
                if (e.remoteCandidates.size())
                    remoteState->remoteCandidates = e.remoteCandidates;
            }
            if (e.fingerprint.isValid()) {
                remoteState->fingerprint = e.fingerprint;
                if (q->isLocal()) {
                    // dtls already created for local transport on remote accept
                    for (auto &c : components) {
                        if (c.dtls)
                            c.dtls->setRemoteFingerprint(e.fingerprint);
                    }
                }
            }
#ifdef JINGLE_SCTP
            if (e.sctpMap.isValid()) {
                remoteState->sctpMap = e.sctpMap;
                remoteState->sctpChannels += e.sctpChannels;
            }
#endif
            if (q->state() == State::Created && q->isRemote()) {
                // initial incoming transport
                q->setState(State::Pending);
            }
            if (q->state() == State::Pending && q->isLocal()) {
                // initial acceptance by remote of the local transport
                q->setState(State::Accepted);
            }
        }

        bool isDataChannelSuppported() const { return Dtls::isSupported(); }

        void notifyRawConnected()
        {
            auto const &acceptors = q->acceptors();
            for (auto const &acceptor : acceptors) {
                if (!(acceptor.features & TransportFeature::DataOriented))
                    ensureRawConnection(acceptor.componentIndex); // TODO signals
            }
            for (auto &c : components) {
                if (c.rawConnection)
                    c.rawConnection->onConnected();
            }
        }

        /**
         * @brief ensureComponentExist adds an ICE component if missed and it's allowed atm for a channel
         *        with given featuers.
         * @param componentIndex  - desired component index (-1 for auto)
         * @return actually allocated component index (e.g. when -1 passed).
         *         returns -1 if something went wrong.
         */
        int ensureComponentExist(int componentIndex, bool lowOverhead = false)
        {
            if (componentIndex == -1) {
                for (auto &c : components)
                    if (!c.initialized) {
                        componentIndex = c.componentIndex;
                        break;
                    }
                if (componentIndex < 0)
                    componentIndex = components.count();
            }

            if (componentIndex >= components.size()) {
                if (ice) {
                    qWarning("Adding channel after negotiation start is not yet supported");
                    return -1;
                }
                for (int i = components.size(); i < componentIndex + 1; i++) {
                    addComponent();
                }
            }
            auto &c       = components[componentIndex];
            c.initialized = true;
            if (lowOverhead)
                c.lowOverhead = true;
            return componentIndex;
        }

        void ensureRawConnection(int componentIndex)
        {
            auto index = quint8(componentIndex);
            if (components[index].rawConnection)
                return;
            components[index].rawConnection = RawConnection::Ptr::create(index);
            if (q->isRemote() && !q->notifyIncomingConnection(components[index].rawConnection))
                components[index].rawConnection.reset();
            // Do we need anything else to do here? connect signals for example?
        }
#ifdef JINGLE_SCTP
        void initSctpAssociation(int componentIndex)
        {
            auto &c = components[componentIndex];
            Q_ASSERT(c.sctp == nullptr);
            c.sctp = new SCTP::Association(q);
            pendingActions |= NewSctpAssociation;
            if (q->wasAccepted() && q->state() != State::ApprovedToSend) // like we already sent our decision
                emit q->updated();
            if (remoteState->sctpMap.isValid()) {
                // TODO if we already have associations params try to ruse them instead of making new one
            }
            q->connect(c.sctp, &SCTP::Association::readyReadOutgoing, q, [this, componentIndex]() {
                auto &c   = components[componentIndex];
                auto  buf = c.sctp->readOutgoing();
                c.dtls->writeDatagram(buf);
            });
            q->connect(c.sctp, &SCTP::Association::newIncomingChannel, q, [this, componentIndex]() {
                qDebug("new incoming sctp channel");
                auto assoc   = components[componentIndex].sctp;
                auto channel = assoc->nextChannel();
                if (!q->notifyIncomingConnection(channel)) {
                    channel->close();
                }
            });
        }

        Connection::Ptr addDataChannel(TransportFeatures channelFeatures, const QString &label, int &componentIndex)
        {
            if (componentIndex == -1)
                componentIndex = 0;

            if (!Dtls::isSupported()) {
                qWarning("An attempt to add a data channel while DTLS is not supported by current configuration");
                return {};
            }
            componentIndex = ensureComponentExist(componentIndex, channelFeatures & TransportFeature::LowOverhead);
            if (componentIndex < 0)
                return {}; // failed to add component for the features

            auto &c = components[componentIndex];
            if (!c.sctp) {
                // basically we can't accept remote transport with own stcp if it wasn't offered.
                // but we can add new associations later with transport-info (undocumented in xep)
                if (q->isRemote() && !q->wasAccepted() && !(remoteState && remoteState->sctpMap.isValid())) {
                    qWarning("remote hasn't negotiated sctp association");
                    return {};
                }
                initSctpAssociation(componentIndex);
            }
            Q_UNUSED(channelFeatures); // TODO
            return c.sctp->newChannel(SCTP::Reliable, true, 0, 256, label);
        }
#endif
    };

    Transport::Transport(const TransportManagerPad::Ptr &pad, Origin creator) :
        XMPP::Jingle::Transport(pad, creator), d(new Private)
    {
        d->q = this;
        d->ensureComponentExist(0);
        d->remoteState.reset(new Element {});
        connect(_pad->manager(), &TransportManager::abortAllRequested, this, [this]() {
            d->aborted = true;
            onFinish(Reason::Cancel);
        });
    }

    Transport::~Transport()
    {
        for (auto &c : d->components) {
#ifdef JINGLE_SCTP
            delete c.sctp;
#endif
            delete c.dtls;
        }
        qDebug("jingle-ice: destroyed");
    }

    void Transport::prepare()
    {
        qDebug("Prepare local offer");
        setState(State::ApprovedToSend);
        auto const &a = acceptors();
        for (auto const &acceptor : a) {
            int ci = acceptor.componentIndex < 0 ? 0 : acceptor.componentIndex;
            d->ensureComponentExist(ci, acceptor.features & TransportFeature::LowOverhead); // it won't fail
            if (acceptor.features & TransportFeature::DataOriented)
                d->components[ci].needDatachannel = true;
        }

        if (Dtls::isSupported()
            && ((isLocal() && _pad->session()->checkPeerCaps(Dtls::FingerPrint::ns()))
                || (isRemote() && d->remoteState->fingerprint.isValid()))) {
            qDebug("initialize DTLS");

            for (auto &c : d->components) {
                d->setupDtls(c.componentIndex);
#ifdef JINGLE_SCTP
                if (isRemote() && c.needDatachannel && !c.sctp) {
                    d->initSctpAssociation(c.componentIndex);
                }
#endif
            }
        }

        auto manager = dynamic_cast<Manager *>(_pad->manager())->d.data();
        Resolver::resolve(this,
                          { { manager->extHost, std::ref(d->extAddr) },
                            { manager->stunBindHost, std::ref(d->stunBindAddr) },
                            { manager->stunRelayUdpHost, std::ref(d->stunRelayUdpAddr) },
                            { manager->stunRelayTcpHost, std::ref(d->stunRelayTcpAddr) } },
                          [this]() {
                              qDebug("resolver finished");
                              d->startIce();
                          });

        emit updated();
    }

    // we got content acceptance from any side and now can connect
    void Transport::start()
    {
        qDebug("Starting connecting");
        setState(State::Connecting);
        d->ice->startChecks();
    }

    bool Transport::update(const QDomElement &transportEl)
    {
        try {
            Element e;
            e.parse(transportEl);
            QTimer::singleShot(0, this, [this, e]() { d->handleRemoteUpdate(e); });
            return true;
        } catch (std::runtime_error &e) {
            qWarning("Transport update failed: %s", e.what());
            return false;
        }
    }

    bool Transport::hasUpdates() const
    {
        return isValid() && d->pendingActions && d->ice && _state >= State::ApprovedToSend
            && !(isRemote() && _state == State::Pending);
    }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate([[maybe_unused]] bool ensureTransportElement = false)
    {
        if (!hasUpdates()) {
            return {};
        }

        qDebug("jingle-ice: taking outgoing update");
        Element e;
        e.ufrag = d->ice->localUfrag();
        e.pwd   = d->ice->localPassword();

        bool hasFingerprint = d->pendingActions & Private::NewFingerprint;
        if (hasFingerprint && d->components[0].dtls) {
            e.fingerprint = d->components[0].dtls->localFingerprint();
        }
        e.candidates        = d->pendingLocalCandidates;
        e.gatheringComplete = d->pendingActions & Private::GatheringComplete;
        if (d->pendingActions & Private::RemoteCandidate)
            e.remoteCandidates = d->ice->selectedCandidates();
        // TODO sctp

        d->pendingLocalCandidates.clear();
        d->pendingActions = 0;

        return OutgoingTransportInfoUpdate { e.toXml(_pad.staticCast<Pad>()->session()->manager()->client()->doc()),
                                             [this, trptr = QPointer<Transport>(d->q), hasFingerprint](bool success) {
                                                 if (!success || !trptr)
                                                     return;
                                                 // if we send our fingerprint as a response to remotely initiated dtls
                                                 // then on response we are sure remote server started dtls server and
                                                 // we can connect now.
                                                 if (hasFingerprint) {
                                                     d->remoteAcceptedFingerprint = true;
                                                 }
                                                 if (hasFingerprint && d->ice && d->ice->canSendMedia())
                                                     for (auto &c : d->components)
                                                         c.dtls->onRemoteAcceptedFingerprint();
                                             } };
    }

    bool Transport::isValid() const { return d != nullptr; }

    TransportFeatures Transport::features() const { return _pad->manager()->features(); }

    int Transport::maxSupportedChannelsPerComponent(TransportFeatures features) const
    {
        return features & TransportFeature::DataOriented ? 65536 : 1;
    };

    void Transport::setComponentsCount(int count)
    {
        if (_state >= State::ApprovedToSend) {
            qWarning("adding component after ICE started is not supported");
            return;
        }
        for (int i = d->components.size(); i < count; i++) {
            d->addComponent();
        }
    }

    // adding ice channels/components (for rtp, rtcp, datachannel etc)
    Connection::Ptr Transport::addChannel(TransportFeatures features, const QString &id, int componentIndex)
    {
#ifdef JINGLE_SCTP
        if (features & TransportFeature::DataOriented)
            return d->addDataChannel(features, id, componentIndex);
#endif
        componentIndex = d->ensureComponentExist(componentIndex, features & TransportFeature::LowOverhead);
        if (componentIndex < 0)
            return {}; // failed to add component for the features
        d->ensureRawConnection(componentIndex);
        auto &channel = d->components[componentIndex].rawConnection;
        channel->setId(id);
        return channel;
    }

    QList<XMPP::Jingle::Connection::Ptr> Transport::channels() const
    {
        QList<XMPP::Jingle::Connection::Ptr> ret;
        for (auto &c : d->components) {
            if (c.rawConnection)
                ret.append(c.rawConnection.staticCast<XMPP::Jingle::Connection>());
#ifdef JINGLE_SCTP
            if (c.sctp)
                ret += c.sctp->channels();
#endif
        }
        return ret;
    }

    //----------------------------------------------------------------
    // Manager
    //----------------------------------------------------------------
    Manager::Manager(QObject *parent) : TransportManager(parent), d(new Private) { }

    Manager::~Manager()
    {
        if (d->jingleManager) {
            d->jingleManager->unregisterTransport(NS);
        }
    }

    TransportFeatures Manager::features() const
    {
        return TransportFeature::HighProbableConnect | TransportFeature::Reliable | TransportFeature::Unreliable
            | TransportFeature::MessageOriented | TransportFeature::LiveOriented
#ifdef JINGLE_SCTP
            | (Dtls::isSupported() ? (TransportFeature::DataOriented | TransportFeature::Ordered) : TransportFeature(0))
#endif
            ;
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { d->jingleManager = jm; }

    QSharedPointer<XMPP::Jingle::Transport> Manager::newTransport(const TransportManagerPad::Ptr &pad, Origin creator)
    {
        return QSharedPointer<Transport>::create(pad, creator).staticCast<XMPP::Jingle::Transport>();
    }

    TransportManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    QStringList Manager::ns() const { return { NS }; }
    QStringList Manager::discoFeatures() const
    {
        return { NS, NS_DTLS
#ifdef JINGLE_SCTP
                 ,
                 SCTP::ns()
#endif
        };
    }

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
