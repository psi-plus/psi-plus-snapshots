// SPDX-License-Identifier: LGPL-2.1-or-later

#include "stunmessage.h"
#include "stuntypes.h"
#include "transportaddress.h"
#include <iris/ice176.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QUdpSocket>
#include <QtCrypto>
#include <functional>
#include <utility>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static bool until(const std::function<bool()> &done)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return done();
}

// A loopback TURN control endpoint. Hold authenticated Allocate replies so the
// test can verify gathering does not finish while a relay is still pending.
class TurnServer {
    struct Pending {
        StunMessage      request;
        TransportAddress client;
    };
    QUdpSocket       socket_;
    QList<Pending>   pending_;
    const QByteArray key_ = QCryptographicHash::hash("test-user:ice-test:test-password", QCryptographicHash::Md5);
    void             reply(const StunMessage &request, const TransportAddress &client, bool reject, bool authenticated)
    {
        StunMessage response;
        response.setClass(reject ? StunMessage::ErrorResponse : StunMessage::SuccessResponse);
        response.setMethod(request.method());
        response.setId(request.id());
        QList<StunMessage::Attribute> attributes;
        if (reject) {
            attributes.append(
                { StunTypes::ERROR_CODE, StunTypes::createErrorCode(authenticated ? 486 : 401, "test response") });
            if (!authenticated) {
                attributes.append({ StunTypes::REALM, "ice-test" });
                attributes.append({ StunTypes::NONCE, "test-nonce" });
            }
        } else {
            if (request.method() != StunTypes::Refresh)
                attributes.append({ StunTypes::XOR_MAPPED_ADDRESS,
                                    StunTypes::createXorMappedAddress(mapped(), response.magic(), response.id()) });
            if (request.method() == StunTypes::Allocate)
                attributes.append({ StunTypes::XOR_RELAYED_ADDRESS,
                                    StunTypes::createXorRelayedAddress(relay(), response.magic(), response.id()) });
            if (request.method() != StunTypes::Binding)
                attributes.append({ StunTypes::LIFETIME,
                                    StunTypes::createLifetime(request.method() == StunTypes::Refresh ? 0 : 600) });
        }
        response.setAttributes(attributes);
        const auto packet
            = response.toBinary(StunMessage::Fingerprint | (authenticated ? StunMessage::MessageIntegrity : 0), key_);
        check(socket_.writeDatagram(packet, client.addr, quint16(client.port)) == packet.size(),
              "TURN fixture send failed");
    }
    void receive()
    {
        while (socket_.hasPendingDatagrams()) {
            QByteArray   packet(int(socket_.pendingDatagramSize()), '\0');
            QHostAddress address;
            quint16      port = 0;
            socket_.readDatagram(packet.data(), packet.size(), &address, &port);
            auto request = StunMessage::fromBinary(packet);
            check(!request.isNull() && request.mclass() == StunMessage::Request, "Unexpected TURN fixture packet");
            const TransportAddress client(address, port);
            if (request.method() == StunTypes::Binding) {
                ++bindings;
                reply(request, client, false, false);
            } else if (!request.hasAttribute(StunTypes::USERNAME)) {
                check(request.method() == StunTypes::Allocate, "Unexpected unauthenticated TURN method");
                reply(request, client, true, false);
            } else {
                request = StunMessage::fromBinary(packet, nullptr,
                                                  StunMessage::MessageIntegrity | StunMessage::Fingerprint, key_);
                check(!request.isNull(), "TURN request integrity check failed");
                if (request.method() == StunTypes::Allocate) {
                    ++allocations;
                    pending_.append({ request, client });
                } else {
                    check(request.method() == StunTypes::Refresh, "Unexpected authenticated TURN method");
                    reply(request, client, false, true);
                }
            }
        }
    }

public:
    int bindings = 0, allocations = 0;
    TurnServer()
    {
        check(socket_.bind(QHostAddress(QHostAddress::LocalHost), 0), "Cannot bind TURN fixture");
        QObject::connect(&socket_, &QUdpSocket::readyRead, &socket_, [this] { receive(); });
    }
    TransportAddress        address() const { return { QHostAddress::LocalHost, socket_.localPort() }; }
    static TransportAddress mapped() { return { QHostAddress("198.51.100.17"), 54321 }; }
    static TransportAddress relay() { return { QHostAddress("192.0.2.55"), 55000 }; }
    void                    complete(bool reject)
    {
        const auto requests = std::exchange(pending_, {});
        for (const auto &pending : requests)
            reply(pending.request, pending.client, reject, true);
    }
};

static void gather(bool expose, bool trickle, bool reject)
{
    qInfo("Gathering: expose=%d trickle=%d reject=%d", expose, trickle, reject);
    TurnServer server;
    Ice176     ice;
    ice.setLocalAddresses({ Ice176::LocalAddress { QHostAddress::LocalHost } });
    ice.setComponentCount(1);
    ice.setAllowIpExposure(expose);
    ice.setLocalFeatures(trickle ? Ice176::Features(Ice176::Trickle) : Ice176::Features());
    ice.setStunBindService(server.address().addr, server.address().port);
    ice.setStunRelayUdpService(server.address().addr, server.address().port, "test-user",
                               QCA::SecureArray("test-password"));
    QList<Ice176::Candidate> candidates;
    QObject::connect(&ice, &Ice176::localCandidatesReady, [&](const QList<Ice176::Candidate> &batch) {
        candidates += batch;
        check(!ice.isLocalGatheringComplete() || expose || reject || !candidates.isEmpty(),
              "Gathering completion preceded relay candidate delivery");
    });
    ice.start(Ice176::Initiator);
    check(until([&] { return server.allocations || ice.isLocalGatheringComplete(); }),
          "No TURN allocation was requested");
    check(server.allocations == 1, "ICE completed gathering without starting TURN");
    check(!ice.isLocalGatheringComplete(), "ICE completed gathering before its pending TURN response");
    server.complete(reject);
    check(until([&] { return ice.isLocalGatheringComplete(); }), "ICE did not finish TURN gathering");
    int relays = 0, hosts = 0, reflexive = 0;
    for (const auto &candidate : candidates) {
        if (candidate.type == "relay") {
            ++relays;
            check(candidate.ip == TurnServer::relay().addr && candidate.port == TurnServer::relay().port,
                  "Wrong relay address");
            if (expose)
                check(candidate.rel_addr == TurnServer::mapped().addr
                          && candidate.rel_port == TurnServer::mapped().port,
                      "Normal gathering lost its related address or port");
            else
                check(candidate.rel_addr.isNull() && candidate.rel_port == -1,
                      "Relay-only candidate exposed the client's address");
        } else if (candidate.type == "host")
            ++hosts;
        else if (candidate.type == "srflx")
            ++reflexive;
        else
            check(false, "Unexpected ICE candidate type");
    }
    check(relays == (reject ? 0 : 1), "Missing or duplicate TURN candidate");
    if (expose)
        check(hosts == 1 && reflexive >= 1 && server.bindings == 1, "Normal ICE gathering regressed");
    else
        check(hosts == 0 && reflexive == 0 && server.bindings == 0,
              "Relay-only ICE exposed a direct candidate or sent STUN binding");
    bool stopped = false;
    QObject::connect(&ice, &Ice176::stopped, [&] { stopped = true; });
    ice.stop();
    check(until([&] { return stopped; }), "ICE did not release its TURN allocation");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    gather(true, true, false);
    gather(false, true, false);
    gather(false, false, false);
    gather(false, true, true);
    qInfo("ICE relay-only gathering and address privacy passed");
    return 0;
}
