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

#include "jingle-ice-connection_p.h"
#include "jingle-ice-group_p.h"
#include "jingle-ice-udp.h"
#include "jingle-ice.h"

#include "dtls.h"
#include "ice176.h"
#include "jingle-session.h"
#include "netnames.h"
#include "stundisco.h"
#include "udpportreserver.h"
#include "xmpp/jid/jid.h"
#include "xmpp_client.h"
#include "xmpp_externalservicediscovery.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_task.h"

#include <memory>

#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QTimer>
#include <QUuid>

template <class T> constexpr std::add_const_t<T> &as_const(T &t) noexcept { return t; }

namespace XMPP { namespace Jingle { namespace ICE {
    const QString NS(QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
    const QString NS_DTLS(QStringLiteral("urn:xmpp:jingle:apps:dtls:0"));

    class Pad::Private {
    public:
        ConnectionRegistry                        registry;
        QMap<ContentKey, QPointer<Transport>>     contentOwners;
        QSet<ContentKey>                          establishedContents;
        std::optional<ConnectionGroupTransaction> stagedGroups;
        std::optional<ConnectionGroupTransaction> replacementGroups;
        QSet<ContentKey>                          replacementContents;
        QSet<ContentKey>                          replacementBound;
        QMap<ContentKey, QPointer<Transport>>     replacementTransports;
        QList<ContentGroup>                       stagedOfferGroups;
        QSet<ContentKey>                          stagedContents;
        bool                                      groupStageAttempted = false;
        bool                                      groupStageFailed    = false;
        bool                                      localAcceptanceKnown = false;
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


    class IceConnection::Runtime {
    public:
        struct Participant {
            QPointer<Transport> transport;
            std::function<void(const QList<Ice176::Candidate> &)> onLocalCandidates;
            std::function<void()>                                 onGatheringComplete;
            std::function<void(Ice176::Error)>                    onError;
            std::function<void()>                                 onRawReady;
            std::function<void()>                                 onFingerprintNeeded;
        };

        QPointer<Pad> pad;
        Origin        creator = Origin::None;

        bool initializationStarted       = false;
        bool remoteFingerprintAccepted  = false;
        bool dtlsAcceptanceStarted       = false;
        bool gatheringComplete           = false;
        bool checksStarted                = false;

        QList<Participant>      participants;
        QList<Ice176::Candidate> localCandidateHistory;

        // Association-owned discovery/startup state. No asynchronous callback
        // below is allowed to depend on one Transport::Private surviving.
        int          basePort = -1;
        bool         allowIpExposure = true;
        QHostAddress selfAddr;
        TurnClient::Proxy stunProxy;

        QHostAddress extAddr;
        QHostAddress stunBindAddr;
        QHostAddress stunRelayUdpAddr;
        QHostAddress stunRelayTcpAddr;
        int          stunBindPort = 0;
        int          stunRelayUdpPort = 0;
        int          stunRelayTcpPort = 0;
        QString      stunRelayUdpUser;
        QString      stunRelayUdpPass;
        QString      stunRelayTcpUser;
        QString      stunRelayTcpPass;

        QString                  remoteUfrag;
        QString                  remotePassword;
        std::optional<Dtls::FingerPrint> remoteFingerprint;
        QList<Ice176::Candidate> remoteCandidates;
        QList<Ice176::SelectedCandidate> remoteSelectedCandidates;
        bool                     remoteGatheringComplete = false;

        void pruneParticipants()
        {
            participants.erase(std::remove_if(participants.begin(), participants.end(),
                                              [](const Participant &p) { return p.transport.isNull(); }),
                               participants.end());
        }

        bool hasParticipant(Transport *transport)
        {
            pruneParticipants();
            return std::any_of(participants.cbegin(), participants.cend(),
                               [transport](const Participant &p) { return p.transport == transport; });
        }

        bool mergeRemoteIce(const Element &e)
        {
            if (!e.ufrag.isEmpty() || !e.pwd.isEmpty()) {
                if ((!remoteUfrag.isEmpty() || !remotePassword.isEmpty())
                    && (remoteUfrag != e.ufrag || remotePassword != e.pwd))
                    return false;
                remoteUfrag    = e.ufrag;
                remotePassword = e.pwd;
            }
            if (e.fingerprint.isValid()) {
                if (remoteFingerprint && *remoteFingerprint != e.fingerprint)
                    return false;
                remoteFingerprint = e.fingerprint;
            }
            if (!e.candidates.isEmpty()) {
                for (const auto &candidate : e.candidates) {
                    const auto duplicate = std::find_if(
                        remoteCandidates.cbegin(), remoteCandidates.cend(), [&candidate](const Ice176::Candidate &known) {
                            return !candidate.id.isEmpty() && candidate.id == known.id;
                        });
                    if (duplicate == remoteCandidates.cend())
                        remoteCandidates.append(candidate);
                }
            }
            if (e.gatheringComplete)
                remoteGatheringComplete = true;
            if (!e.remoteCandidates.isEmpty())
                remoteSelectedCandidates = e.remoteCandidates;
            return true;
        }
    };

    class PreparedIceUpdate final : public XMPP::Jingle::Transport::PreparedUpdate {
    public:
        explicit PreparedIceUpdate(Element value) : element(std::move(value)) { }
        Element element;
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
            auto resolver      = new Resolver(parent); // will be deleted when all finished. see onOneFinished
            resolver->counter  = int(list.size());
            resolver->callback = callback;
            for (auto &item : list) {
                // FIXME hosts may dup in the list. needs optimization
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

            for (Ice176 *ice : std::as_const(left)) {
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
        bool         allowIpExposure = true;

        QString stunBindHost;
        int     stunBindPort = 0;
        QString stunRelayUdpHost;
        int     stunRelayUdpPort = 0;
        QString stunRelayUdpUser;
        QString stunRelayUdpPass;
        QString stunRelayTcpHost;
        int     stunRelayTcpPort = 0;
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

        QList<QNetworkDatagram> datagrams;
        DisconnectReason        disconnectReason = None;
        quint8                  componentIndex   = 0;

        RawConnection(quint8 componentIndex) : componentIndex(componentIndex) { };

        int component() const override { return componentIndex; }

        TransportFeatures features() const override
        {
            return TransportFeature::Fast | TransportFeature::MessageOriented | TransportFeature::HighProbableConnect
                | TransportFeature::Unreliable;
        }

        bool hasPendingDatagrams() const override { return datagrams.size() > 0; }

        QNetworkDatagram readDatagram(qint64 maxSize = -1) override
        {
            Q_UNUSED(maxSize) // TODO or not?
            return datagrams.size() ? datagrams.takeFirst() : QNetworkDatagram();
        }

        qint64 bytesAvailable() const override { return 0; }

        qint64 bytesToWrite() const override { return 0; /*client->bytesToWrite();*/ }

        void close() override { XMPP::Jingle::Connection::close(); }

        qint64 readDataInternal(char *, qint64) override
        {
            qWarning("jingle-ice: called unsupported RawConnection::readDataInternal");
            return -1;
        }

        // RawConnection is an implementation-local endpoint. Packet ingress is
        // association-owned, so the shared ICE runtime must be able to deliver
        // datagrams without routing them through one logical Transport owner.
        void enqueueIncomingUDP(const QByteArray &data)
        {
            datagrams.append(QNetworkDatagram { data });
            emit readyRead();
        }

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

    };

    IceConnection::IceConnection() :
        secureRtpAssociationId(QUuid::createUuid().toRfc4122())
    {
    }

    static void startAssociationDtlsIfReady(IceConnection *network)
    {
        if (!network || !network->runtime || network->runtime->dtlsAcceptanceStarted
            || !network->runtime->remoteFingerprintAccepted || !network->ice || !network->ice->canSendMedia())
            return;
        network->runtime->dtlsAcceptanceStarted = true;
        for (const auto &component : std::as_const(network->components)) {
            if (component.dtls)
                component.dtls->onRemoteAcceptedFingerprint();
        }
    }

    static void startAssociationIce(IceConnection *network)
    {
        if (!network || !network->runtime || network->ice)
            return;
        auto runtime = network->runtime.get();
        auto pad     = runtime->pad.data();
        if (!pad || !pad->session() || !pad->session()->manager() || !pad->session()->manager()->client())
            return;

        if (!runtime->stunBindAddr.isNull() && runtime->stunBindPort > 0)
            qDebug("STUN service: %s;%d", qPrintable(runtime->stunBindAddr.toString()), runtime->stunBindPort);
        if (!runtime->stunRelayUdpAddr.isNull() && runtime->stunRelayUdpPort > 0
            && !runtime->stunRelayUdpUser.isEmpty())
            qDebug("TURN w/ UDP service: %s;%d", qPrintable(runtime->stunRelayUdpAddr.toString()),
                   runtime->stunRelayUdpPort);
        if (!runtime->stunRelayTcpAddr.isNull() && runtime->stunRelayTcpPort > 0
            && !runtime->stunRelayTcpUser.isEmpty())
            qDebug("TURN w/ TCP service: %s;%d", qPrintable(runtime->stunRelayTcpAddr.toString()),
                   runtime->stunRelayTcpPort);

        auto listenAddrs = runtime->selfAddr.isNull() ? Ice176::availableNetworkAddresses()
                                                      : QList<QHostAddress> { runtime->selfAddr };
        QList<Ice176::LocalAddress> localAddrs;
        QStringList                 strList;
        for (const QHostAddress &host : std::as_const(listenAddrs)) {
            Ice176::LocalAddress address;
            address.addr = host;
            localAddrs.append(address);
            strList.append(host.toString());
        }

        if (runtime->basePort != -1) {
            network->portReserver = new UdpPortReserver(network);
            network->portReserver->setAddresses(listenAddrs);
            network->portReserver->setPorts(runtime->basePort, 4);
        }

        if (!strList.isEmpty()) {
            printf("Host addresses:\n");
            for (const QString &host : std::as_const(strList))
                printf("  %s\n", qPrintable(host));
        }

        auto *ice = new Ice176(network);
        network->ice = ice;
        const auto iceGeneration = ++network->generation.iceGeneration;
        ice->setAllowIpExposure(runtime->allowIpExposure);

        auto currentIce = [network, ice, iceGeneration]() {
            return network->ice == ice && network->generation.iceGeneration == iceGeneration;
        };

        QObject::connect(ice, &Ice176::started, network, [network, ice, currentIce]() {
            if (!currentIce())
                return;
            for (const auto &component : std::as_const(network->components)) {
                if (component.lowOverhead)
                    ice->flagComponentAsLowOverhead(component.componentIndex);
            }
        });
        QObject::connect(ice, &Ice176::error, network, [network, currentIce](Ice176::Error error) {
            if (!currentIce() || !network->runtime)
                return;
            network->runtime->pruneParticipants();
            const auto participants = network->runtime->participants;
            for (const auto &participant : participants)
                if (participant.onError)
                    participant.onError(error);
        });
        QObject::connect(ice, &Ice176::localCandidatesReady, network,
                         [network, currentIce](const QList<Ice176::Candidate> &candidates) {
                             if (!currentIce() || !network->runtime)
                                 return;
                             network->runtime->localCandidateHistory += candidates;
                             network->runtime->pruneParticipants();
                             const auto participants = network->runtime->participants;
                             for (const auto &participant : participants)
                                 if (participant.onLocalCandidates)
                                     participant.onLocalCandidates(candidates);
                         });
        QObject::connect(ice, &Ice176::localGatheringComplete, network, [network, currentIce]() {
            if (!currentIce() || !network->runtime)
                return;
            network->runtime->gatheringComplete = true;
            network->runtime->pruneParticipants();
            const auto participants = network->runtime->participants;
            for (const auto &participant : participants)
                if (participant.onGatheringComplete)
                    participant.onGatheringComplete();
        });
        QObject::connect(ice, &Ice176::readyToSendMedia, network, [network, currentIce]() {
            if (!currentIce() || !network->runtime)
                return;
            qDebug("ICE reported ready to send media!");
            if (!network->components.isEmpty() && network->components[0].dtls) {
                startAssociationDtlsIfReady(network);
                return;
            }
            network->runtime->pruneParticipants();
            const auto participants = network->runtime->participants;
            for (const auto &participant : participants)
                if (participant.onRawReady)
                    participant.onRawReady();
        }, Qt::QueuedConnection);
        QObject::connect(ice, &Ice176::readyRead, network, [network, ice, currentIce](int componentIndex) {
            if (!currentIce() || componentIndex < 0 || componentIndex >= network->components.size())
                return;
            auto  buffer    = ice->readDatagram(componentIndex);
            auto &component = network->components[componentIndex];
            if (component.secureRtp) {
                component.secureRtp->receiveMuxed(std::move(buffer));
            } else if (component.dtls) {
                component.dtls->writeIncomingDatagram(buffer);
            } else if (component.rawConnection) {
                component.rawConnection->enqueueIncomingUDP(buffer);
            }
        });

        ice->setProxy(runtime->stunProxy);
        if (network->portReserver)
            ice->setPortReserver(network->portReserver);
        ice->setLocalAddresses(localAddrs);

        if (!runtime->extAddr.isNull()) {
            QList<Ice176::ExternalAddress> external;
            for (const Ice176::LocalAddress &local : std::as_const(localAddrs)) {
                Ice176::ExternalAddress address;
                address.base = local;
                address.addr = runtime->extAddr;
                external.append(address);
            }
            ice->setExternalAddresses(external);
        }

        if (!runtime->stunBindAddr.isNull() && runtime->stunBindPort > 0)
            ice->setStunBindService(runtime->stunBindAddr, runtime->stunBindPort);
        if (!runtime->stunRelayUdpAddr.isNull() && !runtime->stunRelayUdpUser.isEmpty())
            ice->setStunRelayUdpService(runtime->stunRelayUdpAddr, runtime->stunRelayUdpPort,
                                                 runtime->stunRelayUdpUser, runtime->stunRelayUdpPass.toUtf8());
        if (!runtime->stunRelayTcpAddr.isNull() && !runtime->stunRelayTcpUser.isEmpty())
            ice->setStunRelayTcpService(runtime->stunRelayTcpAddr, runtime->stunRelayTcpPort,
                                                 runtime->stunRelayTcpUser, runtime->stunRelayTcpPass.toUtf8());
        ice->setStunDiscoverer(
            pad->session()->manager()->client()->stunDiscoManager()->createMonitor());

        ice->setComponentCount(network->components.count());
        ice->setLocalFeatures(Ice176::Trickle);
        if (!runtime->remoteCandidates.isEmpty()) {
            ice->setRemoteCredentials(runtime->remoteUfrag, runtime->remotePassword);
            ice->addRemoteCandidates(runtime->remoteCandidates);
        }
        if (runtime->remoteGatheringComplete)
            ice->setRemoteGatheringComplete();
        if (!runtime->remoteSelectedCandidates.isEmpty())
            ice->setRemoteSelectedCandidadates(runtime->remoteSelectedCandidates);

        const auto mode = runtime->creator == pad->session()->role() ? Ice176::Initiator : Ice176::Responder;
        ice->start(mode);
    }

    IceConnection::~IceConnection()
    {
        runtime.reset();
        // Resource destruction must not re-enter packet routing while another
        // component is already being torn down.
        if (ice)
            ice->disconnect(this);
        for (const auto &c : components) {
            if (c.secureRtp) {
                // Backend SRTP state belongs to the Jingle media session, not
                // to this ICE QObject. Retire the security association while its
                // Pad listeners are still connected so exported key material is
                // invalidated before we suppress callbacks during destruction.
                c.secureRtp->close();
                c.secureRtp->disconnect();
            }
            if (c.dtls)
                c.dtls->disconnect(this);
#ifdef JINGLE_SCTP
            if (c.sctp)
                c.sctp->disconnect(this);
#endif
        }
        // Tear down packet consumers before handing ICE to asynchronous shutdown.
        for (auto &c : components) {
            delete c.secureRtp;
#ifdef JINGLE_SCTP
            delete c.sctp;
#endif
            delete c.dtls;
        }
        if (ice) {
            auto stopper = new IceStopper;
            stopper->start(portReserver, QList<Ice176 *>() << ice);
        }
    }

    class Transport::Private {
    public:
        enum PendingActions {
            NewCandidate       = 1,
            RemoteCandidate    = 2,
            GatheringComplete  = 4,
            NewFingerprint     = 8,
            NewSctpAssociation = 16
        };

        Transport                     *q                         = nullptr;
        bool                           offerSent                 = false;
        bool                           aborted                   = false;
        bool                           initialOfferReady         = false;
        bool                           remoteAcceptedFingerprint = false;
        quint16                        pendingActions            = 0;
        int                            proxiesInDiscoCount       = 0;
        QList<XMPP::Ice176::Candidate> pendingLocalCandidates; // cid to candidate mapping
        std::unique_ptr<Element>       remoteState;

        // QString            sid;
        // Transport::Mode    mode = Transport::Tcp;
        // QTimer             probingTimer;
        // QTimer             negotiationFinishTimer;
        // QElapsedTimer      lastConnectionStart;
        // size_t             blockSize    = 8192;
        TcpPortDiscoverer *disco = nullptr;
        Resolver           resolver;

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
        QString      stunRelayUdpUser;
        QString      stunRelayUdpPass;
        QString      stunRelayTcpUser;
        QString      stunRelayTcpPass;
        // QString

        // udp stuff
        bool         udpInitialized;
        quint16      udpPort;
        QHostAddress udpAddress;

        ConnectionMembership          membership;
        QSharedPointer<IceConnection> standaloneNetwork;
        QPointer<IceConnection>       network;
        bool                           groupManagedNetwork       = false;
        bool                           networkOwnershipRetired   = false;
        QStringList                    rtpProfiles;

        ~Private() { releaseNetwork(); }

        void releaseNetwork()
        {
            if (network) {
                if (network->runtime) {
                    auto &participants = network->runtime->participants;
                    participants.erase(std::remove_if(participants.begin(), participants.end(),
                                                      [this](const IceConnection::Runtime::Participant &participant) {
                                                          return participant.transport.isNull()
                                                              || participant.transport == q;
                                                      }),
                                       participants.end());
                }
                // No callback capturing this Transport::Private may survive the
                // logical content releasing its association membership.
                if (network->ice)
                    network->ice->disconnect(q);
                for (const auto &component : network->components) {
                    if (component.dtls)
                        component.dtls->disconnect(q);
#ifdef JINGLE_SCTP
                    if (component.sctp)
                        component.sctp->disconnect(q);
#endif
                }
            }
            network.clear();
            groupManagedNetwork = false;
            membership.reset();
            standaloneNetwork.reset();
        }

        inline Jid remoteJid() const { return q->_pad->session()->peer(); }

        bool ensureNetwork()
        {
            if (networkOwnershipRetired)
                return false;
            if (network)
                return true;
            auto pad = q->pad().staticCast<Pad>();
            if (!pad)
                return false;

            bool contentBound  = false;
            bool groupRequired = false;
            network = pad->groupedConnectionFor(q, &contentBound, &groupRequired);
            if (groupRequired) {
                if (!network)
                    return false;
                groupManagedNetwork = true;
            } else {
                membership = pad->membershipFor(q, &contentBound);
                if (contentBound) {
                    if (!membership)
                        return false;
                    network = membership.connection();
                } else {
                    standaloneNetwork = QSharedPointer<IceConnection>::create();
                    network           = standaloneNetwork.data();
                }
            }
            if (!network)
                return false;
            if (!network->runtime)
                network->runtime = std::make_unique<IceConnection::Runtime>();
            if (!network->runtime->pad)
                network->runtime->pad = pad.data();
            if (network->runtime->creator == Origin::None)
                network->runtime->creator = q->creator();

            // A responder may receive and validate all per-content ICE/DTLS
            // signaling before it decides whether to accept an offered BUNDLE.
            // In that case handleRemoteUpdate() intentionally keeps the parsed
            // state on the logical Transport and allocates no physical network.
            // Once the local grouping decision creates the shared association,
            // seed its association-owned runtime immediately so prepare() can
            // configure DTLS from the already received fingerprint.
            if (remoteState && !network->runtime->mergeRemoteIce(*remoteState))
                return false;

            if (!network->runtime->hasParticipant(q)) {
                QPointer<Transport> guard(q);
                IceConnection::Runtime::Participant participant;
                participant.transport = guard;
                participant.onLocalCandidates = [guard](const QList<Ice176::Candidate> &candidates) {
                    if (!guard)
                        return;
                    auto d = guard->d.get();
                    d->pendingActions |= NewCandidate;
                    d->pendingLocalCandidates += candidates;
                    emit guard->updated();
                };
                participant.onGatheringComplete = [guard]() {
                    if (!guard)
                        return;
                    auto d = guard->d.get();
                    if (guard->pad()->ns() == NS)
                        d->pendingActions |= GatheringComplete;
                    if (d->pendingActions)
                        emit guard->updated();
                };
                participant.onError = [guard](Ice176::Error error) {
                    if (guard)
                        guard->onFinish(Reason::Condition::ConnectivityError,
                                        QStringLiteral("ICE failed: %1").arg(error));
                };
                participant.onRawReady = [guard]() {
                    if (guard)
                        guard->d->notifyRawConnected();
                };
                participant.onFingerprintNeeded = [guard]() {
                    if (!guard)
                        return;
                    auto d = guard->d.get();
                    d->pendingActions |= NewFingerprint;
                    emit guard->updated();
                };
                network->runtime->participants.append(std::move(participant));
                if (!network->runtime->localCandidateHistory.isEmpty())
                    network->runtime->participants.last().onLocalCandidates(network->runtime->localCandidateHistory);
                if (network->runtime->gatheringComplete)
                    network->runtime->participants.last().onGatheringComplete();
            }
            if (network->components.isEmpty()) {
                network->components.append(Component {});
                network->components.last().componentIndex = 0;
            }
            return true;
        }

        Component &addComponent()
        {
            network->components.append(Component {});
            auto &c          = network->components.last();
            c.componentIndex = network->components.size() - 1;
            return c;
        }

        bool setupDtls(int componentIndex)
        {
            qDebug("Setup DTLS");
            Q_ASSERT(componentIndex < network->components.length());
            if (network->components[componentIndex].dtls) {
                if (componentIndex == 0)
                    pendingActions |= NewFingerprint;
                return true;
            }
            network->components[componentIndex].dtls
                = new Dtls(network, q->pad()->session()->me().full(), q->pad()->session()->peer().full());

            auto dtls = network->components[componentIndex].dtls;
            // Fingerprint/role negotiation can complete as soon as signaling
            // arrives, but the DTLS engine must not emit handshake records until
            // ICE has a nominated pair. In particular a remote setup=active
            // answer makes us passive/server and setRemoteFingerprint() would
            // otherwise start the server while ICE writes still have no route.
            dtls->setNegotiationDeferred(true);
            if (!rtpProfiles.isEmpty()) {
                if (!dtls->setSRTPProfiles(rtpProfiles))
                    return false;
                auto association = new RTP::SecureRtpAssociation(
                    dtls, network->secureRtpAssociationId, network);
                network->components[componentIndex].secureRtp = association;
                QObject::connect(association, &RTP::SecureRtpAssociation::ready, network,
                                 [net = network](quint64 epoch) {
                                     net->generation.dtlsEpoch = epoch;
                                 });
                QObject::connect(association, &RTP::SecureRtpAssociation::invalidated, network,
                                 [net = network, association](quint64) {
                                     if (association)
                                         net->generation.dtlsEpoch = association->epoch();
                                 });
            }
            if (q->isLocal()) {
                dtls->initOutgoing();
            } else {
                const auto fingerprint = network->runtime ? network->runtime->remoteFingerprint
                                                          : std::optional<Dtls::FingerPrint> {};
                if (!fingerprint)
                    return false;
                dtls->setRemoteFingerprint(*fingerprint);
                dtls->acceptIncoming();
            }

            if (componentIndex == 0) { // for other components it's the same but we don't need multiple fingerprints
                QObject::connect(
                    dtls, &Dtls::needRestart, network,
                    [net = network]() {
                        if (!net->runtime)
                            return;
                        net->runtime->remoteFingerprintAccepted = false;
                        net->runtime->dtlsAcceptanceStarted      = false;
                        net->runtime->pruneParticipants();
                        const auto participants = net->runtime->participants;
                        for (const auto &participant : participants)
                            if (participant.onFingerprintNeeded)
                                participant.onFingerprintNeeded();
                    },
                    Qt::QueuedConnection);
                pendingActions |= NewFingerprint;
            }
            dtls->connect(dtls, &Dtls::readyRead, network, [net = network, componentIndex]() {
                auto &component = net->components[componentIndex];
                auto  d         = component.dtls->readDatagram();
#ifdef JINGLE_SCTP
                if (component.sctp) {
                    // qDebug("sctp write incoming");
                    component.sctp->writeIncoming(d);
                }
#endif
            });
            dtls->connect(dtls, &Dtls::readyReadOutgoing, network, [net = network, componentIndex]() {
                net->ice->writeDatagram(componentIndex, net->components[componentIndex].dtls->readOutgoingDatagram());
            });
            dtls->connect(dtls, &Dtls::connected, network, [net = network, componentIndex, dtls]() {
                qDebug("Dtls::connected");
                auto &c = net->components[componentIndex];
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
            dtls->connect(dtls, &Dtls::errorOccurred, network,
                          [net = network, componentIndex](QAbstractSocket::SocketError error) {
                              qDebug("dtls failed for component %d", componentIndex);
                              auto &c = net->components[componentIndex];
#ifdef JINGLE_SCTP
                              if (c.sctp)
                                  c.sctp->onTransportError(error);
#endif
                              if (c.rawConnection)
                                  c.rawConnection->onError(error);
                          });
            dtls->connect(dtls, &Dtls::closed, network, [net = network, componentIndex]() {
                qDebug("dtls closed for component %d", componentIndex);
                auto &c = net->components[componentIndex];
                if (c.rawConnection)
                    c.rawConnection->onDisconnected(RawConnection::DtlsClosed);
#ifdef JINGLE_SCTP
                if (c.sctp)
                    c.sctp->onTransportClosed();
#endif
            });
            return true;
        }

        void findStunAndTurn()
        {
            if (!network || !network->runtime || network->runtime->initializationStarted)
                return;
            auto runtime = network->runtime.get();
            runtime->initializationStarted = true;

            auto manager          = dynamic_cast<Manager *>(q->pad()->manager())->d.get();
            runtime->basePort     = manager->basePort;
            runtime->allowIpExposure = manager->allowIpExposure;
            runtime->selfAddr     = manager->selfAddr;
            runtime->stunProxy    = manager->stunProxy;
            runtime->stunBindPort = manager->stunBindPort;
            runtime->stunRelayUdpPort = manager->stunRelayUdpPort;
            runtime->stunRelayTcpPort = manager->stunRelayTcpPort;
            runtime->stunRelayUdpUser = manager->stunRelayUdpUser;
            runtime->stunRelayUdpPass = manager->stunRelayUdpPass;
            runtime->stunRelayTcpUser = manager->stunRelayTcpUser;
            runtime->stunRelayTcpPass = manager->stunRelayTcpPass;
            if (!runtime->mergeRemoteIce(*remoteState)) {
                q->onFinish(Reason::FailedTransport, QStringLiteral("Conflicting BUNDLE transport parameters"));
                return;
            }

            auto start = [guard = QPointer<IceConnection>(network)]() {
                if (guard)
                    startAssociationIce(guard.data());
            };

            auto extDisco = q->pad()->session()->manager()->client()->externalServiceDiscovery();
            using namespace std::chrono_literals;
            if (extDisco->isSupported()) {
                extDisco->services(
                    network,
                    [guard = QPointer<IceConnection>(network), start](const ExternalServiceList &services) {
                        if (!guard || !guard->runtime)
                            return;
                        auto runtime = guard->runtime.get();
                        ExternalService::Ptr stun;
                        ExternalService::Ptr turnUdp;
                        ExternalService::Ptr turnTcp;
                        for (const auto &service : services) {
                            if (service->type == QLatin1String("stun")
                                && (service->transport.isEmpty() || service->transport == QLatin1String("udp")))
                                stun = service;
                            else if (service->type == QLatin1String("turn")) {
                                if (service->transport == QLatin1String("tcp"))
                                    turnTcp = service;
                                else
                                    turnUdp = service;
                            }
                        }
                        Resolver::ResolveList resolve;
                        if (stun) {
                            runtime->stunBindAddr.setAddress(stun->host);
                            runtime->stunBindPort = stun->port;
                            if (runtime->stunBindAddr.isNull())
                                resolve.emplace_back(stun->host, std::ref(runtime->stunBindAddr));
                        }
                        if (turnTcp) {
                            runtime->stunRelayTcpAddr.setAddress(turnTcp->host);
                            runtime->stunRelayTcpPort = turnTcp->port;
                            runtime->stunRelayTcpUser = turnTcp->username;
                            runtime->stunRelayTcpPass = turnTcp->password;
                            if (runtime->stunRelayTcpAddr.isNull())
                                resolve.emplace_back(turnTcp->host, std::ref(runtime->stunRelayTcpAddr));
                        }
                        if (turnUdp) {
                            runtime->stunRelayUdpAddr.setAddress(turnUdp->host);
                            runtime->stunRelayUdpPort = turnUdp->port;
                            runtime->stunRelayUdpUser = turnUdp->username;
                            runtime->stunRelayUdpPass = turnUdp->password;
                            if (runtime->stunRelayUdpAddr.isNull())
                                resolve.emplace_back(turnUdp->host, std::ref(runtime->stunRelayUdpAddr));
                        }
                        if (resolve.empty())
                            start();
                        else
                            Resolver::resolve(guard.data(), std::move(resolve), start);
                    },
                    5min, { "stun", "turn" });
                return;
            }

            runtime->extAddr.setAddress(manager->extHost);
            runtime->stunBindAddr.setAddress(manager->stunBindHost);
            runtime->stunRelayUdpAddr.setAddress(manager->stunRelayUdpHost);
            runtime->stunRelayTcpAddr.setAddress(manager->stunRelayTcpHost);
            Resolver::ResolveList resolve {
                { manager->extHost, std::ref(runtime->extAddr) },
                { manager->stunBindHost, std::ref(runtime->stunBindAddr) },
                { manager->stunRelayUdpHost, std::ref(runtime->stunRelayUdpAddr) },
                { manager->stunRelayTcpHost, std::ref(runtime->stunRelayTcpAddr) }
            };
            Resolver::resolve(network, std::move(resolve), start);
        }

        void setupRemoteICE(const Element &e)
        {
            Q_ASSERT(network->ice != nullptr);
            if (!e.candidates.isEmpty()) {
                network->ice->setRemoteCredentials(e.ufrag, e.pwd);
                network->ice->addRemoteCandidates(e.candidates);
            }
            if (e.gatheringComplete) {
                network->ice->setRemoteGatheringComplete();
            }
            if (e.remoteCandidates.size()) {
                network->ice->setRemoteSelectedCandidadates(e.remoteCandidates);
            }
        }

        void handleRemoteUpdate(const Element &e)
        {
            if (q->state() == State::Finished)
                return;

            // Signaling state belongs to this per-content cursor and can arrive
            // before the responder has chosen its BUNDLE answer. Preserve it
            // without forcing association allocation.
            if (!e.candidates.isEmpty() || !e.ufrag.isEmpty()) {
                remoteState->ufrag = e.ufrag;
                remoteState->pwd   = e.pwd;
            }
            if (e.gatheringComplete)
                remoteState->gatheringComplete = true;
            remoteState->candidates += e.candidates;
            if (!e.remoteCandidates.isEmpty())
                remoteState->remoteCandidates = e.remoteCandidates;
            if (e.fingerprint.isValid())
                remoteState->fingerprint = e.fingerprint;
#ifdef JINGLE_SCTP
            if (e.sctpMap.isValid()) {
                remoteState->sctpMap = e.sctpMap;
                remoteState->sctpChannels += e.sctpChannels;
            }
#endif

            auto pad = q->pad().staticCast<Pad>();
            if (pad && pad->shouldDeferGroupedNetwork(q)) {
                if (q->state() == State::Created && q->isRemote())
                    q->setState(State::Pending);
                return;
            }

            if (!ensureNetwork()) {
                q->onFinish(Reason::FailedTransport, QStringLiteral("Unable to allocate ICE association"));
                return;
            }
            if (network->runtime && !network->runtime->mergeRemoteIce(e)) {
                q->onFinish(Reason::FailedTransport, QStringLiteral("Conflicting BUNDLE transport parameters"));
                return;
            }

            if (network->ice)
                setupRemoteICE(e);

            if (e.fingerprint.isValid() && q->isLocal() && network->runtime
                && network->runtime->remoteFingerprint) {
                for (auto &component : network->components) {
                    if (component.dtls)
                        component.dtls->setRemoteFingerprint(*network->runtime->remoteFingerprint);
                }
            }
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
            for (auto &c : network->components) {
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
            if (!rtpProfiles.isEmpty()) {
                if (componentIndex > 0)
                    return -1;
                componentIndex = 0;
            }
            if (componentIndex == -1) {
                for (auto &c : network->components)
                    if (!c.initialized) {
                        componentIndex = c.componentIndex;
                        break;
                    }
                if (componentIndex < 0)
                    componentIndex = network->components.count();
            }

            if (componentIndex >= network->components.size()) {
                if (network->ice) {
                    qWarning("Adding channel after negotiation start is not yet supported");
                    return -1;
                }
                for (int i = network->components.size(); i < componentIndex + 1; i++) {
                    addComponent();
                }
            }
            auto &c       = network->components[componentIndex];
            c.initialized = true;
            if (lowOverhead)
                c.lowOverhead = true;
            return componentIndex;
        }

        void ensureRawConnection(int componentIndex)
        {
            auto index = quint8(componentIndex);
            if (network->components[index].rawConnection)
                return;
            network->components[index].rawConnection = RawConnection::Ptr::create(index);
            if (q->isRemote() && !q->notifyIncomingConnection(network->components[index].rawConnection))
                network->components[index].rawConnection.reset();
            // Do we need anything else to do here? connect signals for example?
        }
#ifdef JINGLE_SCTP
        void initSctpAssociation(int componentIndex)
        {
            auto &c = network->components[componentIndex];
            Q_ASSERT(c.sctp == nullptr);
            c.sctp = new SCTP::Association(network);
            pendingActions |= NewSctpAssociation;
            if (q->wasAccepted() && q->state() != State::ApprovedToSend) // like we already sent our decision
                emit q->updated();
            if (remoteState->sctpMap.isValid()) {
                // TODO if we already have associations params try to ruse them instead of making new one
            }
            QObject::connect(c.sctp, &SCTP::Association::readyReadOutgoing, network,
                             [net = network, componentIndex]() {
                                 auto &c   = net->components[componentIndex];
                                 auto  buf = c.sctp->readOutgoing();
                                 c.dtls->writeDatagram(buf);
                             });
            q->connect(c.sctp, &SCTP::Association::newIncomingChannel, q, [this, componentIndex]() {
                qDebug("new incoming sctp channel");
                auto assoc   = network->components[componentIndex].sctp;
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

            auto &c = network->components[componentIndex];
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
        // The association is selected lazily after Application::setTransport()
        // has made the owning Jingle content observable to the session-local Pad.
        d->remoteState.reset(new Element {});
        connect(this, &XMPP::Jingle::Transport::stateChanged, this, [this]() {
            if (!d->groupManagedNetwork && _state >= State::Finishing) {
                if (auto binding = rtpAssociation())
                    binding->close();
            }
        });
        connect(this, &XMPP::Jingle::Transport::failed, this, [this]() {
            if (!d->groupManagedNetwork) {
                if (auto binding = rtpAssociation())
                    binding->close();
            }
        });
        connect(_pad->manager(), &TransportManager::abortAllRequested, this, [this]() {
            d->aborted = true;
            onFinish(Reason::Cancel);
        });
    }

    Transport::~Transport()
    {
        if (auto binding = rtpAssociation()) {
            binding->disconnect(this);
            if (!d->groupManagedNetwork)
                binding->close();
        }
    }

    void Transport::releaseNetworkOwnership()
    {
        // This Transport has been superseded for its logical content. It may
        // remain alive in selector/signaling bookkeeping, but asynchronous work
        // owned by the previous incarnation must never reacquire an association.
        d->networkOwnershipRetired = true;
        if (auto binding = rtpAssociation()) {
            binding->disconnect(this);
            if (!d->groupManagedNetwork)
                binding->close();
        }
        d->releaseNetwork();
    }

    bool Transport::iceCanSendMedia() const
    {
        return d->network && d->network->ice && d->network->ice->canSendMedia();
    }

    void Transport::stop()
    {
        XMPP::Jingle::Transport::stop();
        if (!d->groupManagedNetwork) {
            if (auto binding = rtpAssociation())
                binding->close();
        }
    }

    bool Transport::enableRtpMux(const QStringList &profiles)
    {
        if (profiles.isEmpty() || !d->ensureNetwork())
            return false;
        if ((_state != State::Created && !(isRemote() && _state == State::Pending))
            || d->network->components.size() != 1 || d->network->components[0].rawConnection)
            return false;
        if (!d->groupManagedNetwork && (d->network->ice || d->network->components[0].dtls))
            return false;
        if (d->groupManagedNetwork && d->network->components[0].dtls
            && !d->network->components[0].secureRtp)
            return false;

        const auto supported = Dtls::supportedSRTPProfiles();
        for (const auto &profile : profiles)
            if (!supported.contains(profile))
                return false;

        d->rtpProfiles                         = profiles;
        d->network->components[0].lowOverhead = true;
        return true;
    }

    RTP::SecureRtpAssociation *Transport::rtpAssociation() const
    {
        return !d->network || d->network->components.isEmpty() ? nullptr
                                                                : d->network->components[0].secureRtp;
    }

    bool Transport::sendProtectedRtpPacket(QByteArray packet, RTP::PacketKind kind, quint64 epoch)
    {
        auto association = rtpAssociation();
        if (!association || !d->network || !d->network->ice || _state < State::Connecting
            || _state >= State::Finishing
            || !association->validateProtectedMuxed(packet, kind, epoch))
            return false;
        d->network->ice->writeDatagram(0, packet);
        return true;
    }

    void Transport::prepare()
    {
        qDebug("Prepare local offer");
        if (!d->ensureNetwork()) {
            onFinish(Reason::FailedTransport, QStringLiteral("Unable to allocate ICE association"));
            return;
        }
        if (!d->rtpProfiles.isEmpty() && isRemote() && !d->remoteState->fingerprint.isValid()) {
            onFinish(Reason::SecurityError, QStringLiteral("RTP requires a DTLS fingerprint"));
            return;
        }
        QPointer<Transport> guard(this);
        setState(State::ApprovedToSend);
        if (!guard || _state != State::ApprovedToSend)
            return;
        auto const &a = acceptors();
        for (auto const &acceptor : a) {
            if (!d->rtpProfiles.isEmpty() && !(acceptor.features & TransportFeature::DataOriented)) {
                onFinish(Reason::FailedTransport, QStringLiteral("Raw channels are not allowed on protected RTP"));
                return;
            }
            int ci = acceptor.componentIndex < 0 ? 0 : acceptor.componentIndex;
            if (d->ensureComponentExist(ci, acceptor.features & TransportFeature::LowOverhead) < 0) {
                onFinish(Reason::FailedTransport, QStringLiteral("Incompatible ICE component"));
                return;
            }
            if (acceptor.features & TransportFeature::DataOriented)
                d->network->components[ci].needDatachannel = true;
        }

        if (Dtls::isSupported()
            && ((!d->rtpProfiles.isEmpty()) || (isLocal() && _pad->session()->checkPeerCaps(Dtls::FingerPrint::ns()))
                || (isRemote() && d->remoteState->fingerprint.isValid()))) {
            qDebug("initialize DTLS");

            for (auto &c : d->network->components) {
                if (!d->setupDtls(c.componentIndex)) {
                    onFinish(Reason::SecurityError, QStringLiteral("DTLS-SRTP profile configuration failed"));
                    return;
                }
#ifdef JINGLE_SCTP
                if (isRemote() && c.needDatachannel && !c.sctp) {
                    d->initSctpAssociation(c.componentIndex);
                }
#endif
            }
        }

        d->findStunAndTurn();
        emit updated();
    }

    // we got content acceptance from any side and now can connect
    void Transport::start()
    {
        qDebug("Starting connecting");
        if (_state >= State::Finishing)
            return;
        if (!d->network || !d->network->ice) {
            onFinish(Reason::FailedTransport, QStringLiteral("ICE transport has not been prepared"));
            return;
        }
        if (d->groupManagedNetwork && !pad().staticCast<Pad>()->groupedConnectionAccepted(this)) {
            onFinish(Reason::FailedTransport, QStringLiteral("Negotiated BUNDLE membership changed before ICE start"));
            return;
        }
        QPointer<Transport> guard(this);
        setState(State::Connecting);
        if (guard && _state == State::Connecting) {
            if (!d->network->runtime || !d->network->runtime->checksStarted) {
                if (d->network->runtime)
                    d->network->runtime->checksStarted = true;
                d->network->ice->startChecks();
            }
        }
    }

    Transport::PrepareUpdateResult Transport::prepareUpdate(const QDomElement &transportEl)
    {
        try {
            QDomDocument normalizedDoc;
            QDomElement  normalized = transportEl;
            if (_pad->ns() == NS_ICE_UDP) {
                QString error;
                normalized = iceUdpToInternal(normalizedDoc, transportEl, NS, &error);
                if (normalized.isNull()) {
                    qWarning("ICE-UDP transport update failed: %s", qPrintable(error));
                    return { PrepareUpdateStatus::Invalid, {}, {} };
                }
            }

            Element element;
            element.parse(normalized);
            return { PrepareUpdateStatus::Ready, std::make_unique<PreparedIceUpdate>(std::move(element)), {} };
        } catch (const std::runtime_error &e) {
            qWarning("Transport update failed: %s", e.what());
            return { PrepareUpdateStatus::Invalid, {}, {} };
        }
    }

    bool Transport::commitPreparedUpdate(PreparedUpdatePtr update)
    {
        auto prepared = dynamic_cast<PreparedIceUpdate *>(update.get());
        if (!prepared || d->networkOwnershipRetired)
            return false;

        // Preserve the existing deferred ICE application boundary. The caller has
        // already validated the complete signaling batch before this work is queued.
        // A transport-replace may retire this Transport before the event loop runs;
        // in that case the old payload belongs to a dead signaling incarnation and
        // must not recreate a standalone association.
        const auto element       = prepared->element;
        const auto network       = QPointer<IceConnection>(d->network);
        const auto iceGeneration = network ? network->generation.iceGeneration : 0;
        QTimer::singleShot(0, this, [this, element, network, iceGeneration]() {
            if (d->networkOwnershipRetired)
                return;
            if (network
                && (!d->network || d->network != network
                    || network->generation.iceGeneration != iceGeneration))
                return;
            d->handleRemoteUpdate(element);
        });
        return true;
    }

    bool Transport::update(const QDomElement &transportEl)
    {
        auto prepared = prepareUpdate(transportEl);
        return prepared && commitPreparedUpdate(std::move(prepared.update));
    }

    bool Transport::hasUpdates() const
    {
        return isValid() && d->network && d->pendingActions && d->network->ice && _state >= State::ApprovedToSend
            && !(isRemote() && _state == State::Pending)
            && (d->network->ice->isLocalGatheringComplete() || d->pendingLocalCandidates.size());
    }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate([[maybe_unused]] bool ensureTransportElement = false)
    {
        if (!hasUpdates()) {
            return {};
        }

        qDebug("jingle-ice: taking outgoing update");
        Element e;
        e.ufrag = d->network->ice->localUfrag();
        e.pwd   = d->network->ice->localPassword();

        bool hasFingerprint = d->pendingActions & Private::NewFingerprint;
        if (hasFingerprint && d->network->components[0].dtls) {
            e.fingerprint = d->network->components[0].dtls->localFingerprint();
        }
        e.candidates        = d->pendingLocalCandidates;
        e.gatheringComplete = d->pendingActions & Private::GatheringComplete;
        if (d->pendingActions & Private::RemoteCandidate)
            e.remoteCandidates = d->network->ice->selectedCandidates();
        // TODO sctp

        auto        doc          = _pad.staticCast<Pad>()->session()->manager()->client()->doc();
        QDomElement transportXml = e.toXml(doc);
        if (_pad->ns() == NS_ICE_UDP) {
            QString error;
            transportXml = internalToIceUdp(*doc, transportXml, &error);
            if (transportXml.isNull()) {
                qWarning("Failed to serialize ICE-UDP transport update: %s", qPrintable(error));
                return {};
            }
        }

        d->pendingLocalCandidates.clear();
        d->pendingActions = 0;

        return OutgoingTransportInfoUpdate { transportXml,
                                             [this, trptr = QPointer<Transport>(d->q), hasFingerprint](Task *task) {
                                                 if (!trptr || !task || !task->success())
                                                     return;
                                                 // if we send our fingerprint as a response to remotely initiated dtls
                                                 // then on response we are sure remote server started dtls server and
                                                 // we can connect now.
                                                 if (hasFingerprint && d->network && d->network->runtime) {
                                                     d->remoteAcceptedFingerprint = true;
                                                     d->network->runtime->remoteFingerprintAccepted = true;
                                                     startAssociationDtlsIfReady(d->network);
                                                 }
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
        if (!d->ensureNetwork())
            return;
        if (!d->rtpProfiles.isEmpty() && count != 1)
            return; // This mode has explicitly negotiated RTCP multiplexing.
        if (_state >= State::ApprovedToSend) {
            qWarning("adding component after ICE started is not supported");
            return;
        }
        for (int i = d->network->components.size(); i < count; i++) {
            d->addComponent();
        }
    }

    // adding ice channels/components (for rtp, rtcp, datachannel etc)
    Connection::Ptr Transport::addChannel(TransportFeatures features, const QString &id, int componentIndex)
    {
        if (!d->ensureNetwork())
            return {};
#ifdef JINGLE_SCTP
        if (features & TransportFeature::DataOriented)
            return d->addDataChannel(features, id, componentIndex);
#endif
        if (!d->rtpProfiles.isEmpty())
            return {}; // Protected RTP uses sendProtectedRtpPacket(), never a raw channel.
        componentIndex = d->ensureComponentExist(componentIndex, features & TransportFeature::LowOverhead);
        if (componentIndex < 0)
            return {}; // failed to add component for the features
        d->ensureRawConnection(componentIndex);
        auto &channel = d->network->components[componentIndex].rawConnection;
        channel->setId(id);
        return channel;
    }

    QList<XMPP::Jingle::Connection::Ptr> Transport::channels() const
    {
        QList<XMPP::Jingle::Connection::Ptr> ret;
        if (!d->network)
            return ret;
        for (auto &c : d->network->components) {
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
            d->jingleManager->unregisterTransport(NS_ICE_UDP);
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

    QStringList Manager::ns() const { return { NS, NS_ICE_UDP }; }
    QStringList Manager::discoFeatures() const
    {
        QStringList features { NS, NS_ICE_UDP };
        if (Dtls::isSupported()) {
            features += NS_DTLS;
#ifdef JINGLE_SCTP
            features += SCTP::ns();
#endif
        }
        return features;
    }

    void Manager::setBasePort(int port) { d->basePort = port; }

    void Manager::setExternalAddress(const QString &host) { d->extHost = host; }

    void Manager::setSelfAddress(const QHostAddress &addr) { d->selfAddr = addr; }

    void Manager::setAllowIpExposure(bool allow) { d->allowIpExposure = allow; }

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
    Pad::Pad(Manager *manager, Session *session) : d(std::make_unique<Private>()), _manager(manager), _session(session)
    {
        // TcpPortReserver is an optional client facility. ICE currently keeps
        // the legacy TCP discovery scope only as an integration hook, so a
        // headless/embedded Client without a reserver must still be usable.
        if (auto reserver = _session->manager()->client()->tcpPortReserver())
            _discoScope = reserver->scope(QString::fromLatin1("ice"));
    }

    Pad::~Pad() = default;

    QString Pad::ns() const
    {
        const auto requested = requestedNamespace();
        return requested.isEmpty() ? NS : requested;
    }

    static std::optional<ContentKey> contentForTransport(Session *session, Transport *transport)
    {
        if (!session || !transport)
            return std::nullopt;
        std::optional<ContentKey> content;
        for (auto it = session->contentList().cbegin(); it != session->contentList().cend(); ++it) {
            if (it.value() && it.value()->transport().data() == transport) {
                if (content)
                    return std::nullopt;
                content = it.key();
            }
        }
        return content;
    }

    static bool sameGroupMembers(const ContentGroup &left, const ContentGroup &right)
    {
        if (left.semantics != QLatin1String("BUNDLE") || right.semantics != QLatin1String("BUNDLE")
            || left.contents.size() != right.contents.size())
            return false;
        QSet<QString> names;
        for (const auto &name : left.contents)
            names.insert(name);
        return names.size() == left.contents.size()
            && std::all_of(right.contents.cbegin(), right.contents.cend(),
                           [&names](const QString &name) { return names.contains(name); });
    }

    IceConnection *Pad::groupedConnectionFor(Transport *transport, bool *contentBound, bool *groupRequired)
    {
        if (contentBound)
            *contentBound = false;
        if (groupRequired)
            *groupRequired = false;
        if (!transport || transport->pad().data() != this || !_session)
            return nullptr;

        const auto content = contentForTransport(_session, transport);
        if (!content)
            return nullptr;
        if (contentBound)
            *contentBound = true;

        if (!d->groupStageAttempted) {
            d->groupStageAttempted = true;
            const bool provisionalOffer = _session->role() == Origin::Initiator;
            d->stagedOfferGroups = provisionalOffer ? _session->groupings() : _session->remoteGroupings();
            const auto stagedAnswerGroups = provisionalOffer ? d->stagedOfferGroups : _session->groupings();

            bool hasSharedOffer = false;
            for (const auto &group : std::as_const(d->stagedOfferGroups))
                hasSharedOffer |= group.semantics == QLatin1String("BUNDLE") && group.contents.size() > 1;

            if (hasSharedOffer) {
                QList<GroupNegotiation::Member> members;
                for (auto it = _session->contentList().cbegin(); it != _session->contentList().cend(); ++it) {
                    auto app = it.value();
                    auto tr  = app ? app->transport() : QSharedPointer<XMPP::Jingle::Transport>();
                    if (!app || !tr || !tr->pad())
                        continue;
                    members.append(GroupNegotiation::Member { it.key(), tr->pad()->ns(),
                                                               app->supportsSharedTransport(), std::nullopt });
                }

                GroupNegotiation::Error error = GroupNegotiation::Error::None;
                auto plan
                    = GroupNegotiation::initialPlan(members, d->stagedOfferGroups, stagedAnswerGroups, &error);
                if (!plan) {
                    d->groupStageFailed = true;
                } else {
                    auto staged = ConnectionGroupTransaction::stageBundled(*plan, d->registry, ns());
                    if (!staged) {
                        d->groupStageFailed = true;
                    } else {
                        d->stagedGroups = std::move(*staged);
                        for (const auto &association : plan->associations()) {
                            if (!association.bundled || association.members.size() < 2
                                || association.transportNamespace != ns())
                                continue;
                            for (const auto &key : association.members) {
                                d->stagedContents.insert(key);
                                auto app = _session->content(key.first, key.second);
                                if (app) {
                                    connect(app, &QObject::destroyed, this, [this, key]() {
                                        if (d->replacementContents.contains(key)) {
                                            d->replacementGroups.reset();
                                            d->replacementContents.clear();
                                            d->replacementBound.clear();
                                            d->replacementTransports.clear();
                                        }
                                        if (d->stagedGroups)
                                            d->stagedGroups->release(key);
                                        d->contentOwners.remove(key);
                                        d->establishedContents.remove(key);
                                        d->registry.prune();
                                    });
                                }
                            }
                        }
                    }
                }
            }
        }

        const bool offeredAsShared = std::any_of(
            d->stagedOfferGroups.cbegin(), d->stagedOfferGroups.cend(), [content](const ContentGroup &group) {
                return group.semantics == QLatin1String("BUNDLE") && group.contents.size() > 1
                    && group.contents.contains(content->first);
            });
        // Initiator-side staging is provisional until the peer answer arrives.
        // A responder already knows its local answer before accept()/prepare(),
        // so any refused member must stay on an independent association.
        const bool requiresShared = d->stagedContents.contains(*content)
            || (_session->role() == Origin::Initiator && offeredAsShared);
        if (groupRequired)
            *groupRequired = requiresShared;
        if (!requiresShared || d->groupStageFailed || !d->stagedGroups)
            return nullptr;

        const bool replacingEstablished = d->establishedContents.contains(*content)
            && d->contentOwners.value(*content) != transport;

        if (replacingEstablished) {
            const auto &negotiatedGroups
                = _session->role() == Origin::Initiator ? _session->remoteGroupings() : _session->groupings();
            std::optional<ContentGroup> negotiatedBundle;
            for (const auto &group : negotiatedGroups) {
                if (group.semantics == QLatin1String("BUNDLE") && group.contents.size() > 1
                    && group.contents.contains(content->first)) {
                    if (negotiatedBundle)
                        return nullptr;
                    negotiatedBundle = group;
                }
            }
            if (!negotiatedBundle)
                return nullptr;

            if (!d->replacementGroups) {
                const auto oldAssociationId = d->stagedGroups->associationIdFor(*content);
                if (!oldAssociationId)
                    return nullptr;

                QList<GroupNegotiation::Member> members;
                QSet<ContentKey>                 replacementKeys;
                for (const auto &name : negotiatedBundle->contents) {
                    std::optional<ContentKey> key;
                    Application                *app = nullptr;
                    for (auto it = _session->contentList().cbegin(); it != _session->contentList().cend(); ++it) {
                        if (it.key().first != name)
                            continue;
                        if (key)
                            return nullptr;
                        key = it.key();
                        app = it.value();
                    }
                    if (!key || !app || !d->stagedContents.contains(*key)
                        || d->stagedGroups->associationIdFor(*key) != oldAssociationId)
                        return nullptr;
                    auto current = app->transport();
                    if (!current || current->pad().data() != this || d->contentOwners.value(*key) == current.data())
                        return nullptr; // never split one live BUNDLE generation
                    members.append(GroupNegotiation::Member { *key, current->pad()->ns(),
                                                               app->supportsSharedTransport(), std::nullopt });
                    replacementKeys.insert(*key);
                }

                GroupNegotiation::Error error = GroupNegotiation::Error::None;
                auto plan = GroupNegotiation::initialPlan(
                    members, QList<ContentGroup> { *negotiatedBundle },
                    QList<ContentGroup> { *negotiatedBundle }, &error);
                if (!plan)
                    return nullptr;
                auto replacement = ConnectionGroupTransaction::stageBundledReplacement(
                    *plan, d->registry, *d->stagedGroups, ns());
                if (!replacement)
                    return nullptr;

                d->replacementGroups   = std::move(*replacement);
                d->replacementContents = replacementKeys;
                for (const auto &key : std::as_const(d->replacementContents)) {
                    auto app = _session->content(key.first, key.second);
                    auto tr  = app ? qSharedPointerDynamicCast<Transport>(app->transport()) : QSharedPointer<Transport>();
                    if (!tr) {
                        d->replacementGroups.reset();
                        d->replacementContents.clear();
                        return nullptr;
                    }
                    d->replacementTransports.insert(key, tr.data());
                    connect(tr.data(), &QObject::destroyed, this, [this, key, raw = tr.data()]() {
                        if (d->replacementTransports.value(key) != raw)
                            return;
                        d->replacementGroups.reset();
                        d->replacementContents.clear();
                        d->replacementBound.clear();
                        d->replacementTransports.clear();
                        d->registry.prune();
                    });
                }
            }

            if (!d->replacementContents.contains(*content)
                || d->replacementTransports.value(*content) != transport)
                return nullptr;

            auto replacementConnection = d->replacementGroups->connectionFor(*content);
            if (!replacementConnection)
                return nullptr;
            d->replacementBound.insert(*content);

            if (d->replacementBound == d->replacementContents) {
                if (!d->replacementGroups->activateReplacement(d->registry)) {
                    d->replacementGroups.reset();
                    d->replacementContents.clear();
                    d->replacementBound.clear();
                    d->replacementTransports.clear();
                    return nullptr;
                }
                if (!d->replacementGroups->finalizeReplacement(d->registry)) {
                    d->replacementGroups->rollbackReplacement(d->registry);
                    d->replacementGroups.reset();
                    d->replacementContents.clear();
                    d->replacementBound.clear();
                    d->replacementTransports.clear();
                    return nullptr;
                }

                auto previousGroups = std::move(d->stagedGroups);
                d->replacementGroups->retainUnreplacedFrom(std::move(*previousGroups), d->replacementContents);
                d->stagedGroups = std::move(d->replacementGroups);
                d->replacementGroups.reset();

                for (const auto &key : std::as_const(d->replacementContents)) {
                    auto next = d->replacementTransports.value(key);
                    auto previous = d->contentOwners.value(key);
                    if (previous && previous != next)
                        previous->releaseNetworkOwnership();
                    d->contentOwners.insert(key, next);
                    d->establishedContents.insert(key);
                    if (next) {
                        connect(next, &QObject::destroyed, this, [this, key, raw = next.data()]() {
                            if (d->contentOwners.value(key) == raw)
                                d->contentOwners.remove(key);
                            d->registry.prune();
                        });
                    }
                }

                d->replacementContents.clear();
                d->replacementBound.clear();
                d->replacementTransports.clear();
                previousGroups.reset();
                d->registry.prune();
            }
            return replacementConnection;
        }

        auto connection = d->stagedGroups->connectionFor(*content);
        if (!connection)
            return nullptr;

        auto previous = d->contentOwners.value(*content);
        if (previous && previous != transport)
            previous->releaseNetworkOwnership();
        d->contentOwners.insert(*content, transport);
        d->establishedContents.insert(*content);
        connect(transport, &QObject::destroyed, this, [this, content = *content, transport]() {
            if (d->contentOwners.value(content) == transport)
                d->contentOwners.remove(content);
            d->registry.prune();
        });
        return connection;
    }

    bool Pad::groupedConnectionAccepted(Transport *transport) const
    {
        if (!transport || !_session || !d->stagedGroups)
            return true;
        const auto content = contentForTransport(_session, transport);
        if (!content || !d->stagedContents.contains(*content))
            return true;

        if (_session->role() == Origin::Responder)
            return true; // staged directly from the responder's actual local answer

        const auto &answer = _session->remoteGroupings();
        for (const auto &offered : d->stagedOfferGroups) {
            if (offered.semantics != QLatin1String("BUNDLE") || !offered.contents.contains(content->first)
                || offered.contents.size() < 2)
                continue;
            return std::any_of(answer.cbegin(), answer.cend(),
                               [&offered](const ContentGroup &accepted) { return sameGroupMembers(offered, accepted); });
        }
        return false;
    }

    bool Pad::shouldDeferGroupedNetwork(Transport *transport) const
    {
        if (!transport || !_session || _session->role() != Origin::Responder || d->localAcceptanceKnown)
            return false;
        const auto content = contentForTransport(_session, transport);
        if (!content)
            return false;
        return std::any_of(_session->remoteGroupings().cbegin(), _session->remoteGroupings().cend(),
                           [content](const ContentGroup &group) {
                               return group.semantics == QLatin1String("BUNDLE") && group.contents.size() > 1
                                   && group.contents.contains(content->first);
                           });
    }

    ConnectionMembership Pad::membershipFor(Transport *transport, bool *contentBound)
    {
        if (contentBound)
            *contentBound = false;
        if (!transport || transport->pad().data() != this || !_session)
            return {};

        std::optional<ContentKey> content;
        for (auto it = _session->contentList().cbegin(); it != _session->contentList().cend(); ++it) {
            if (it.value() && it.value()->transport().data() == transport) {
                if (content)
                    return {}; // one Transport cannot own two logical contents
                content = it.key();
            }
        }
        if (!content)
            return {}; // compatibility path for direct low-level Transport users
        if (contentBound)
            *contentBound = true;
        if (d->stagedContents.contains(*content))
            return {};

        // A transport-replace is an ownership transition for this logical
        // content. The replaced Transport may survive in a selector backup, but
        // it must no longer retain the active association.
        auto previous = d->contentOwners.value(*content);
        if (previous && previous != transport)
            previous->releaseNetworkOwnership();
        d->contentOwners.remove(*content);
        d->registry.prune();

        auto membership = d->registry.create(*content);
        if (!membership)
            return {};

        d->contentOwners.insert(*content, transport);
        connect(transport, &QObject::destroyed, this, [this, content = *content, transport]() {
            if (d->contentOwners.value(content) == transport)
                d->contentOwners.remove(content);
            d->registry.prune();
        });
        return membership;
    }

    qsizetype Pad::liveAssociationCount() const
    {
        return d ? d->registry.liveAssociationCount() : 0;
    }

    Session *Pad::session() const { return _session; }

    TransportManager *Pad::manager() const { return _manager; }

    void Pad::onLocalAccepted()
    {
        d->localAcceptanceKnown = true;
    }

} // namespace Ice
} // namespace Jingle
} // namespace XMPP

#include "jingle-ice.moc"
