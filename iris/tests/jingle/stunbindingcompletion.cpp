// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QtCore>
#include <QtCrypto>
#include <QtNetwork>
#include <memory>

// Access the real binding's transaction to drive its existing timeout slot
// without waiting for network retries or changing the public API.
#define private public
#include "../../src/irisnet/noncore/stunbinding.cpp"
#undef private

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static void timeout(StunTransaction *transaction)
{
    auto *state = transaction->children().value(0);
    check(state && QMetaObject::invokeMethod(state, "t_timeout", Qt::DirectConnection),
          "could not drive the real STUN transaction timeout");
}

static void completion(bool success, bool deleteInCallback, bool releaseBeforeCallback)
{
    // TCP takes the terminal timeout branch on the first timeout.
    auto                          pool = StunTransactionPool::Ptr::create(StunTransaction::Tcp);
    QPointer<StunTransactionPool> poolGuard(pool.data());
    auto                         *binding = new StunBinding(pool.data());
    QPointer<StunBinding>         bindingGuard(binding);
    const TransportAddress        remote(QHostAddress::LocalHost, 8010);
    const TransportAddress        mapped(QHostAddress::LocalHost, 8020);
    StunMessage                   request;
    QObject::connect(
        pool.data(), &StunTransactionPool::outgoingMessage,
        [&](const QByteArray &packet, const TransportAddress &) { request = StunMessage::fromBinary(packet); });
    int  completions = 0;
    auto completed   = [&]() {
        ++completions;
        check(bool(poolGuard) && bool(bindingGuard), "completion lost its pool or binding before notification");
        check(!binding->d->trans, "completed transaction prevented binding reuse");
        pool.clear();
        if (deleteInCallback)
            delete binding;
    };
    QObject::connect(binding, &StunBinding::success, [&]() {
        check(success && binding->reflexiveAddress() == mapped, "binding reported the wrong mapped address");
        completed();
    });
    QObject::connect(binding, &StunBinding::error, [&](StunBinding::Error error) {
        check(!success && error == StunBinding::ErrorTimeout, "binding reported the wrong error");
        check(binding->errorString() == QStringLiteral("Request timed out."), "timeout diagnostic was lost");
        completed();
    });
    binding->start(remote);
    QPointer<StunTransaction> transaction(binding->d->trans.get());
    check(!request.isNull(), "binding did not create a STUN request");
    if (releaseBeforeCallback)
        pool.clear();
    if (success) {
        StunMessage response;
        response.setClass(StunMessage::SuccessResponse);
        response.setMethod(StunTypes::Binding);
        response.setId(request.id());
        response.setAttributes({ { StunTypes::MAPPED_ADDRESS, StunTypes::createMappedAddress(mapped) } });
        check(poolGuard->writeIncomingMessage(response, remote), "matching STUN response was rejected");
    } else {
        timeout(transaction);
    }
    check(completions == 1, "binding did not complete exactly once");
    check(!transaction && !poolGuard && !bindingGuard, "completion leaked the transaction, pool or binding");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static void restartInCallback()
{
    auto                          pool = StunTransactionPool::Ptr::create(StunTransaction::Tcp);
    QPointer<StunTransactionPool> poolGuard(pool.data());
    auto                         *binding = new StunBinding(pool.data());
    QPointer<StunBinding>         bindingGuard(binding);
    int                           errors = 0;
    QObject::connect(binding, &StunBinding::error, [&](StunBinding::Error error) {
        check(error == StunBinding::ErrorTimeout, "restarted binding reported the wrong error");
        if (++errors == 1)
            binding->start(TransportAddress(QHostAddress::LocalHost, 8010));
    });
    binding->start(TransportAddress(QHostAddress::LocalHost, 8010));
    QPointer<StunTransaction> first(binding->d->trans.get());
    pool.clear();
    timeout(first);
    check(!first && bindingGuard && poolGuard, "old completion destroyed a restarted binding");
    QPointer<StunTransaction> second(binding->d->trans.get());
    check(bool(second), "completion callback could not restart the binding");
    timeout(second);
    check(errors == 2 && !second && !bindingGuard && !poolGuard, "restarted binding leaked or lost completion");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static void cancelOrDelete(bool cancel, bool releaseBeforeRetirement)
{
    auto                          pool = StunTransactionPool::Ptr::create(StunTransaction::Tcp);
    QPointer<StunTransactionPool> poolGuard(pool.data());
    auto                         *binding = new StunBinding(pool.data());
    QPointer<StunBinding>         bindingGuard(binding);
    int                           errors = 0;
    QObject::connect(binding, &StunBinding::error, [&](StunBinding::Error) { ++errors; });
    binding->start(TransportAddress(QHostAddress::LocalHost, 8010));
    QPointer<StunTransaction> transaction(binding->d->trans.get());
    if (releaseBeforeRetirement)
        pool.clear();
    if (cancel) {
        binding->cancel();
        pool.clear();
        timeout(transaction);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    } else {
        delete binding;
        pool.clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    check(errors == 0 && !transaction && !bindingGuard && !poolGuard,
          "retired binding emitted a callback or kept its transaction/pool alive");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    for (bool success : { false, true }) {
        for (bool deleteInCallback : { false, true }) {
            for (bool releaseBeforeCallback : { true, false })
                completion(success, deleteInCallback, releaseBeforeCallback);
        }
    }
    restartInCallback();
    for (bool cancel : { false, true }) {
        for (bool releaseBeforeRetirement : { false, true })
            cancelOrDelete(cancel, releaseBeforeRetirement);
    }
    qInfo("STUN terminal callback lifetime regressions passed");
}
