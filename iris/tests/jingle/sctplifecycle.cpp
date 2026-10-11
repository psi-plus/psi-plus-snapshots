#include "jingle-sctp.h"
#include "jingle-webrtc-datachannel_p.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QQueue>
#include <QThread>

#include <functional>

using namespace XMPP::Jingle;
using namespace XMPP::Jingle::SCTP;

namespace {

// Production SCTP signals one outgoing packet at a time. Model its direct
// one-packet reader, retaining packets only in the simulated network wire.
class PacketWire {
public:
    explicit PacketWire(Association &source)
    {
        connection_ = QObject::connect(&source, &Association::readyReadOutgoing, &source, [this, &source]() {
            if (source.pendingOutgoingDatagrams() != 1)
                qFatal("SCTP producer batched packets behind one readiness notification");
            packets.enqueue(source.readOutgoing());
        });
    }
    ~PacketWire() { QObject::disconnect(connection_); }
    QQueue<QByteArray> packets;

private:
    QMetaObject::Connection connection_;
};

void movePackets(PacketWire &from, Association &to)
{
    while (!from.packets.isEmpty()) {
        const auto packet = from.packets.dequeue();
        if (!packet.isEmpty())
            to.writeIncoming(packet);
    }
}

bool pumpUntil(PacketWire &leftWire, Association &left, PacketWire &rightWire, Association &right,
               const std::function<bool()> &done, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        movePackets(leftWire, right);
        movePackets(rightWire, left);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    movePackets(leftWire, right);
    movePackets(rightWire, left);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

int fail(const char *message)
{
    qCritical("%s", message);
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Association left(nullptr);
    Association right(nullptr);
    PacketWire  leftWire(left), rightWire(right);
    left.setIdSelector(IdSelector::Even);
    right.setIdSelector(IdSelector::Odd);

    auto leftOne = left.newChannel(Reliable, true, 0, 256, QStringLiteral("one"), QStringLiteral("ft"));
    auto leftTwo = left.newChannel(Reliable, true, 0, 256, QStringLiteral("two"), QStringLiteral("ft"));
    if (!leftOne || !leftTwo)
        return fail("failed to create local data channels");

    int incomingCount = 0;
    QObject::connect(&right, &Association::newIncomingChannel, &app, [&incomingCount]() { ++incomingCount; });

    left.onTransportConnected();
    right.onTransportConnected();

    if (!pumpUntil(leftWire, left, rightWire, right,
                   [&]() { return incomingCount == 2 && leftOne->isOpen() && leftTwo->isOpen(); }))
        return fail("SCTP data channels did not open");

    if (right.pendingChannels() != 2)
        return fail("unexpected incoming data-channel count");

    auto remoteA = right.nextChannel().staticCast<WebRTCDataChannel>();
    auto remoteB = right.nextChannel().staticCast<WebRTCDataChannel>();
    if (!remoteA || !remoteB)
        return fail("failed to obtain incoming data channels");

    QSharedPointer<WebRTCDataChannel> remoteOne;
    QSharedPointer<WebRTCDataChannel> remoteTwo;
    if (remoteA->label == QLatin1String("one")) {
        remoteOne = remoteA;
        remoteTwo = remoteB;
    } else {
        remoteOne = remoteB;
        remoteTwo = remoteA;
    }
    if (remoteOne->label != QLatin1String("one") || remoteTwo->label != QLatin1String("two"))
        return fail("incoming data-channel labels do not match");
    if (remoteOne->protocol != QLatin1String("ft") || remoteTwo->protocol != QLatin1String("ft"))
        return fail("incoming data-channel protocols do not match");
    if ((remoteOne->channelType & 0x80) || (remoteTwo->channelType & 0x80))
        return fail("ordered data channels were encoded as unordered");
    if (remoteOne->priority != 256 || remoteOne->reliability != 0)
        return fail("DCEP priority and reliability fields were interchanged");

    // content-add creates a channel on an already established association.
    // Exercise both allocation parities without reconnecting SCTP.
    // Fill both send buffers first: DCEP control must retry, not be lost to
    // EAGAIN or exposed to the application as binary payload.
    const QByteArray congestion(262144, 'x');
    if (!leftTwo->writeDatagram(QNetworkDatagram(congestion))
        || !remoteTwo->writeDatagram(QNetworkDatagram(congestion)))
        return fail("could not fill SCTP send buffers before channel addition");
    auto lateLeft  = left.newDeferredChannel(QStringLiteral("late-left"));
    auto lateRight = right.newChannel(Reliable, true, 0, 256, QStringLiteral("late-right"), QStringLiteral("ft"));
    if (!lateLeft || !lateRight || !pumpUntil(leftWire, left, rightWire, right, [&]() {
            return lateRight->isOpen() && left.pendingChannels() == 1;
        }))
        return fail("opposite parity late channel did not open");
    if (lateLeft->isOpen() || right.pendingChannels() != 0)
        return fail("deferred channel opened before content acceptance");
    left.activateChannel(lateLeft);
    if (!lateLeft || !lateRight || !pumpUntil(leftWire, left, rightWire, right, [&]() {
            return lateLeft->isOpen() && lateRight->isOpen() && left.pendingChannels() == 1
                && right.pendingChannels() == 1;
        }))
        return fail("channels added to active SCTP did not complete DCEP");
    auto remoteLateLeft  = right.nextChannel().staticCast<WebRTCDataChannel>();
    auto remoteLateRight = left.nextChannel().staticCast<WebRTCDataChannel>();
    if (!remoteLateLeft || !remoteLateRight || remoteLateLeft->label != QLatin1String("late-left")
        || remoteLateRight->label != QLatin1String("late-right"))
        return fail("late channels were routed to the wrong peer");
    const QByteArray latePayload("new-content-payload");
    lateLeft->writeDatagram(QNetworkDatagram(latePayload));
    lateRight->writeDatagram(QNetworkDatagram(latePayload));
    if (!pumpUntil(leftWire, left, rightWire, right,
                   [&]() { return remoteLateLeft->hasPendingDatagrams() && remoteLateRight->hasPendingDatagrams(); })
        || remoteLateLeft->readDatagram().data() != latePayload
        || remoteLateRight->readDatagram().data() != latePayload)
        return fail("late channels did not deliver payload");
    if (!leftTwo->hasPendingDatagrams() || !remoteTwo->hasPendingDatagrams()
        || leftTwo->readDatagram().data() != congestion || remoteTwo->readDatagram().data() != congestion)
        return fail("channel handshakes lost or changed existing payload");

    // Model the FT finishing boundary precisely. The application gets
    // bytesWritten when a block has entered usrsctp, not when the peer has
    // received it. Queue a sizeable reliable tail that still fits entirely in
    // the SCTP send buffer, verify the application-facing queue is empty, and
    // then request stream close before any of those packets are pumped to the
    // peer. Stream reset must not discard that accepted tail.
    QList<QByteArray> tails;
    for (int i = 0; i < 16; ++i) {
        QByteArray tail(8192, char(i));
        tails.append(tail);
        if (!leftOne->writeDatagram(QNetworkDatagram(tail)))
            return fail("failed to queue buffered tail on first channel");
    }
    if (leftOne->bytesToWrite() != 0)
        return fail("test tail did not enter the SCTP send buffer");

    int localCloseFinished  = 0;
    int remoteCloseFinished = 0;
    QObject::connect(leftOne.data(), &ByteStream::delayedCloseFinished, &app,
                     [&localCloseFinished]() { ++localCloseFinished; });
    QObject::connect(remoteOne.data(), &ByteStream::connectionClosed, &app,
                     [&remoteCloseFinished]() { ++remoteCloseFinished; });

    leftOne->close();

    if (!pumpUntil(leftWire, left, rightWire, right, [&]() {
            return left.channels().size() == 3 && right.channels().size() == 3 && localCloseFinished == 1;
        }))
        return fail("SCTP stream reset did not complete local per-stream close");

    if (remoteCloseFinished != 0)
        return fail("remote close completed before buffered data was drained");

    for (const auto &tail : std::as_const(tails)) {
        if (!remoteOne->hasPendingDatagrams())
            return fail("stream close discarded accepted SCTP tail data");
        if (remoteOne->readDatagram().data() != tail)
            return fail("buffered tail changed across stream close");
        if (remoteCloseFinished != 0)
            return fail("remote close notification overtook accounting of final datagram");
    }
    if (remoteOne->hasPendingDatagrams())
        return fail("unexpected extra datagram after buffered tail");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (remoteCloseFinished != 1)
        return fail("remote close did not complete after final datagram returned");

    // A seek cancels a content while one write is still outside usrsctp.
    // Drop that unaccepted write; preserve accepted bytes and unblock siblings.
    const QByteArray acceptedTail(262144, 'a');
    const QByteArray cancelledTail(8192, 'b');
    lateLeft->writeDatagram(QNetworkDatagram(acceptedTail));
    lateLeft->writeDatagram(QNetworkDatagram(cancelledTail));
    if (!lateLeft->bytesToWrite())
        return fail("cancel fixture did not queue an unaccepted SCTP write");
    lateLeft->close();
    if (lateLeft->bytesToWrite())
        return fail("cancelled stream retained unaccepted application writes");
    if (!pumpUntil(leftWire, left, rightWire, right,
                   [&]() { return left.channels().size() == 2 && right.channels().size() == 2; })
        || !remoteLateLeft->hasPendingDatagrams() || remoteLateLeft->readDatagram().data() != acceptedTail
        || remoteLateLeft->hasPendingDatagrams())
        return fail("cancelled stream lost accepted bytes or resent unaccepted tail");

    const QByteArray survivor("surviving-channel");
    if (!leftTwo->writeDatagram(QNetworkDatagram(survivor)))
        return fail("failed to write surviving channel");
    if (!pumpUntil(leftWire, left, rightWire, right, [&]() { return remoteTwo->hasPendingDatagrams(); }))
        return fail("surviving channel stopped after sibling close");
    if (remoteTwo->readDatagram().data() != survivor)
        return fail("surviving channel payload mismatch");

    return 0;
}
