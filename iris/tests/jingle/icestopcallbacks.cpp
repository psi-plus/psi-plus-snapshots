// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QtCore>
#include <QtCrypto>
#include <QtNetwork>
// Exercise retained STUN transactions deterministically without waiting for a
// real network timeout or adding private transport state to the public API.
#define private public
#include "../../src/irisnet/noncore/ice176.cpp"
#include <iris/ice176.h>
#undef private
using namespace XMPP;
static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}
static void stoppedCallbacks()
{
    Ice176 ice;
    auto   d           = ice.d;
    auto   pair        = Ice176::Private::CandidatePair::Ptr::create();
    pair->pool         = StunTransactionPool::Ptr::create(StunTransaction::Udp);
    pair->binding      = new StunBinding(pair->pool.data());
    pair->local        = IceComponent::CandidateInfo::Ptr::create();
    pair->remote       = IceComponent::CandidateInfo::Ptr::create();
    pair->local->addr  = TransportAddress(QHostAddress::LocalHost, 9998);
    pair->remote->addr = TransportAddress(QHostAddress::LocalHost, 9999);
    pair->isValid      = true;
    pair->state        = Ice176::Private::PInProgress;
    // A retained valid pair is deliberately absent from the main checklist.
    d->checkList.validPairs.append(pair);
    d->state   = Ice176::Private::Started;
    int writes = 0, errors = 0, ready = 0;
    QObject::connect(pair->pool.data(), &StunTransactionPool::outgoingMessage, d,
                     [&](const QByteArray &, const TransportAddress &) { ++writes; });
    StunBinding *binding = pair->binding;
    QObject::connect(binding, &StunBinding::error, d, [&](StunBinding::Error) { ++errors; });
    QObject::connect(&ice, &Ice176::error, [&](Ice176::Error) { ++errors; });
    QObject::connect(&ice, &Ice176::readyToSendMedia, [&] { ++ready; });
    pair->binding->start(pair->remote->addr);
    check(writes > 0, "STUN transaction did not start");
    const int before = writes;
    ice.stop();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    check(ice.isStopped(), "ICE did not finish stopping");
    const auto stoppedPairState = pair->state;
    // Late signals from a retired transaction must be disconnected;
    // retaining its pair must not keep an outgoing transport callback alive.
    emit pair->pool->outgoingMessage(QByteArrayLiteral("late"), pair->remote->addr);
    emit pair->binding->error(StunBinding::ErrorTimeout);
    emit pair->binding->success();
    check(writes == before && errors == 0 && ready == 0, "retired STUN callbacks escaped ICE stop");
    check(pair->state == stoppedPairState, "late result mutated retained pair state");
    // A cancelled binding can start another transaction immediately.
    binding->start(pair->remote->addr);
    binding->cancel();
}

class FakeTransport : public IceTransport {
public:
    void       stop() override { }
    bool       hasPendingDatagrams(int) const override { return false; }
    QByteArray readDatagram(int, TransportAddress &) override { return {}; }
    void       writeDatagram(int, const QByteArray &packet, const TransportAddress &) override
    {
        request = StunMessage::fromBinary(packet);
    }
    StunMessage request;
    void        addChannelPeer(const TransportAddress &) override { }
    void        setDebugLevel(DebugLevel) override { }
    void        changeThread(QThread *) override { }
};

static void discardedPairRetiresBinding()
{
    auto pair     = Ice176::Private::CandidatePair::Ptr::create();
    pair->pool    = StunTransactionPool::Ptr::create(StunTransaction::Udp);
    pair->binding = new StunBinding(pair->pool.data());
    QPointer<StunBinding>         binding(pair->binding);
    QPointer<StunTransactionPool> pool(pair->pool.data());
    pair->binding->start(TransportAddress(QHostAddress::LocalHost, 9999));
    pair.clear();
    check(!binding && !pool, "discarded ICE pair left its STUN check running");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static void replacementRetiresOldBinding()
{
    Ice176 ice;
    auto   d                 = ice.d;
    auto   transport         = QSharedPointer<FakeTransport>::create();
    auto   pair              = Ice176::Private::CandidatePair::Ptr::create();
    pair->local              = IceComponent::CandidateInfo::Ptr::create();
    pair->remote             = IceComponent::CandidateInfo::Ptr::create();
    pair->local->addr        = TransportAddress(QHostAddress::LocalHost, 9998);
    pair->local->base        = pair->local->addr;
    pair->remote->addr       = TransportAddress(QHostAddress::LocalHost, 9999);
    pair->local->componentId = pair->remote->componentId = 1;
    IceComponent::Candidate candidate;
    candidate.info         = pair->local;
    candidate.iceTransport = transport;
    d->localCandidates.append(candidate);
    d->checkList.pairs.append(pair);
    d->components.emplace_back();
    d->components.back().id = 1;
    d->components.back().ic = new IceComponent(1);
    d->state                = Ice176::Private::Started;
    d->mode                 = Ice176::Initiator;
    d->checkPair(pair);
    QPointer<StunBinding>         oldBinding(pair->binding);
    QPointer<StunTransactionPool> oldPool(pair->pool.data());
    const auto                    oldRequest = transport->request;
    d->checkPair(pair);
    check(!oldBinding && !oldPool, "new ICE check left the old STUN transaction running");
    check(pair->binding && pair->pool && pair->state == Ice176::Private::PInProgress,
          "retiring old check damaged its replacement");
    StunMessage staleResponse;
    staleResponse.setClass(StunMessage::SuccessResponse);
    staleResponse.setMethod(StunTypes::Binding);
    staleResponse.setId(oldRequest.id());
    staleResponse.setAttributes({ { StunTypes::MAPPED_ADDRESS, StunTypes::createMappedAddress(pair->local->addr) } });
    check(!pair->pool->writeIncomingMessage(staleResponse, pair->remote->addr),
          "old STUN response was accepted by the replacement pool");
    check(pair->state == Ice176::Private::PInProgress, "old STUN response mutated the replacement pair");
    delete d->components.back().ic;
    d->components.clear();
    ice.stop();
    QCoreApplication::sendPostedEvents();
}

// A link-local remote candidate paired with several local interfaces is reached through each pair's own
// interface; the shared remote candidate keeps no zone of its own.
static void linkLocalPairsKeepTheirScope()
{
    Ice176 ice;
    auto   d            = ice.d;
    auto   remote       = IceComponent::CandidateInfo::Ptr::create();
    remote->addr        = TransportAddress(QHostAddress(QStringLiteral("fe80::2")), 9999);
    remote->componentId = 1;
    QList<QSharedPointer<Ice176::Private::CandidatePair>> pairs;
    for (const auto &zone : { QStringLiteral("lan0"), QStringLiteral("vpn0") }) {
        auto local         = IceComponent::CandidateInfo::Ptr::create();
        local->addr        = TransportAddress(QHostAddress(QStringLiteral("fe80::1%") + zone), 9998);
        local->componentId = 1;
        pairs += d->makeCandidatesPair(local, remote);
    }
    check(pairs.size() == 2 && pairs[0] && pairs[1], "link-local candidates were not paired");
    check(pairs[0]->remote->addr.addr.scopeId() == QLatin1String("lan0")
              && pairs[1]->remote->addr.addr.scopeId() == QLatin1String("vpn0"),
          "a link-local pair is not reached through its own interface");
    check(remote->addr.addr.scopeId().isEmpty(), "pairing changed the shared remote candidate");
}

static void peerReflexiveResponse()
{
    Ice176        ice;
    auto          d         = ice.d;
    auto          transport = QSharedPointer<FakeTransport>::create();
    FakeTransport otherTransport;
    auto          pair       = Ice176::Private::CandidatePair::Ptr::create();
    pair->local              = IceComponent::CandidateInfo::Ptr::create();
    pair->remote             = IceComponent::CandidateInfo::Ptr::create();
    pair->local->addr        = TransportAddress(QHostAddress("192.168.50.155"), 53459);
    pair->local->base        = TransportAddress(QHostAddress::LocalHost, 9998);
    pair->local->type        = IceComponent::PeerReflexiveType;
    pair->local->componentId = pair->remote->componentId = 1;
    pair->remote->addr                                   = TransportAddress(QHostAddress::LocalHost, 9999);
    pair->state                                          = Ice176::Private::PInProgress;
    pair->isValid                                        = true;
    IceComponent::Candidate candidate;
    candidate.info         = pair->local;
    candidate.iceTransport = transport;
    candidate.path         = 0;
    d->localCandidates.append(candidate);
    d->checkList.validPairs.append(pair);
    d->state = Ice176::Private::Started;

    d->components.emplace_back();
    d->components.back().id = 1;
    d->components.back().ic = new IceComponent(1);
    d->mode                 = Ice176::Initiator;
    pair->finalNomination   = true;
    d->checkPair(pair);
    StunBinding *binding = pair->binding;
    // Isolate transaction routing from higher-level nomination scheduling,
    // exercised by the real loopback ICE tests.
    binding->disconnect(d);
    int successes = 0;
    QObject::connect(binding, &StunBinding::success, [&] { ++successes; });
    auto request = transport->request;
    check(!request.isNull(), "nomination request did not start");
    check(request.hasAttribute(StunTypes::USE_CANDIDATE), "request is not a nomination");
    StunMessage response;
    response.setClass(StunMessage::SuccessResponse);
    response.setMethod(StunTypes::Binding);
    response.setId(request.id());
    response.setAttributes({ { StunTypes::MAPPED_ADDRESS, StunTypes::createMappedAddress(pair->local->addr) } });

    // This helper receives authenticated responses from it_readyRead. Neither
    // another socket/path nor another remote endpoint may finish nomination.
    d->handleCheckResponse(&otherTransport, 0, response, pair->remote->addr);
    d->handleCheckResponse(transport.data(), 1, response, pair->remote->addr);
    d->handleCheckResponse(transport.data(), 0, response, TransportAddress(QHostAddress::LocalHost, 10000));
    StunMessage unrelated = response;
    QByteArray  wrongId(reinterpret_cast<const char *>(request.id()), 12);
    wrongId[0] = char(wrongId[0] ^ 1);
    unrelated.setId(reinterpret_cast<const quint8 *>(wrongId.constData()));
    d->handleCheckResponse(transport.data(), 0, unrelated, pair->remote->addr);
    check(successes == 0, "response from another ICE path or peer was accepted");
    d->handleCheckResponse(transport.data(), 0, response, pair->remote->addr);
    check(successes == 1, "valid peer-reflexive nomination response was lost");
    delete d->components.back().ic;
    d->components.clear();
    ice.stop();
    QCoreApplication::sendPostedEvents();
}

static void usableConnectivityCancelsPacTimeout()
{
    Ice176 ice;
    auto   d = ice.d;

    d->state         = Ice176::Private::Started;
    d->localFeatures = Ice176::NotNominatedData;
    d->components.emplace_back();
    auto &component         = d->components.back();
    component.id            = 1;
    component.hasValidPairs = true;

    auto pair                 = Ice176::Private::CandidatePair::Ptr::create();
    pair->local               = IceComponent::CandidateInfo::Ptr::create();
    pair->remote              = IceComponent::CandidateInfo::Ptr::create();
    pair->local->addr         = TransportAddress(QHostAddress::LocalHost, 10000);
    pair->local->base         = pair->local->addr;
    pair->local->componentId  = 1;
    pair->local->type         = IceComponent::HostType;
    pair->remote->addr        = TransportAddress(QHostAddress::LocalHost, 10001);
    pair->remote->componentId = 1;
    pair->remote->type        = IceComponent::HostType;
    component.highestPair     = pair;

    d->pacTimer = std::make_unique<QTimer>(d);
    d->pacTimer->setSingleShot(true);
    d->pacTimer->start(1000);

    int ready = 0;
    QObject::connect(&ice, &Ice176::readyToSendMedia, [&] { ++ready; });
    d->tryReadyToSendMedia();

    check(ice.canSendMedia(), "valid pair did not make ICE writable");
    check(ready == 1, "ICE readiness signal was not emitted exactly once");
    check(!d->pacTimer, "PAC timer survived usable ICE connectivity");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    stoppedCallbacks();
    discardedPairRetiresBinding();
    replacementRetiresOldBinding();
    linkLocalPairsKeepTheirScope();
    peerReflexiveResponse();
    usableConnectivityCancelsPacTimeout();
    qInfo("ICE peer-reflexive response, PAC and stopped callback regressions passed");
}
