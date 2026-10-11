#include "jingle-sctp-association_p.h"

#include <QCoreApplication>
#include <QPointer>

#include <memory>

using namespace XMPP::Jingle;
using namespace XMPP::Jingle::SCTP;

namespace {

int fail(const char *message)
{
    qCritical("%s", message);
    return 1;
}

enum class Closure { SctpClosed, SctpFailed, TransportClosed, TransportError };

int checkClosure(Closure closure)
{
    Association owner(nullptr);
    auto        state         = std::make_unique<AssociationPrivate>(&owner);
    auto        pending       = state->newChannel(Reliable, true, 0, 256, {}, {}).staticCast<WebRTCDataChannel>();
    state->transportConnected = true;
    auto reader               = state->newChannel(Reliable, true, 0, 256, {}, {}).staticCast<WebRTCDataChannel>();
    auto writer               = state->newChannel(Reliable, true, 0, 256, {}, {}).staticCast<WebRTCDataChannel>();
    if (!pending || !reader || !writer)
        return fail("failed to create closure fixture channels");
    reader->open(QIODevice::ReadWrite);
    writer->open(QIODevice::ReadWrite);
    const QByteArray payload("received-before-association-closed");
    reader->onIncomingData(payload, PPID_BINARY);
    writer->closeRequested  = true;
    writer->outgoingBufSize = 17;
    state->outgoingMessageQueue.enqueue(
        { writer, { quint16(writer->streamId), 0, PPID_BINARY, 0, QByteArray(17, 'x') } });
    state->outgoingPacketsQueue.enqueue(QByteArray("unsent-wire-packet"));

    int pendingClosed = 0, readerClosed = 0, writerClosed = 0, readerDisconnected = 0;
    QObject::connect(pending.data(), &ByteStream::connectionClosed, &owner, [&] { ++pendingClosed; });
    QObject::connect(reader.data(), &ByteStream::connectionClosed, &owner, [&] { ++readerClosed; });
    QObject::connect(reader.data(), &Connection::disconnected, &owner, [&] { ++readerDisconnected; });
    QObject::connect(writer.data(), &ByteStream::delayedCloseFinished, &owner, [&] { ++writerClosed; });

    // Inject the production listener edge instead of waiting minutes for the
    // retransmission timeout. SCTP callbacks must return before notifying users.
    switch (closure) {
    case Closure::SctpClosed:
        state->OnSctpAssociationClosed(&state->assoc);
        break;
    case Closure::SctpFailed:
        state->OnSctpAssociationFailed(&state->assoc);
        break;
    case Closure::TransportClosed:
        state->onTransportClosed();
        break;
    case Closure::TransportError:
        state->onTransportError(QAbstractSocket::NetworkError);
        break;
    }
    if ((closure == Closure::SctpClosed || closure == Closure::SctpFailed)
        && (pendingClosed || readerDisconnected || writerClosed))
        return fail("SCTP closure notification ran inside the usrsctp callback");
    QCoreApplication::processEvents();
    if (pendingClosed != 1 || writerClosed != 1 || readerDisconnected != 1 || readerClosed)
        return fail("association closure did not notify all channels or discarded buffered input");
    if (reader->isWritable() || !reader->isReadable() || writer->bytesToWrite())
        return fail("closed association retained writable channels or an outgoing queue");
    if (!state->allChannels().isEmpty() || !state->pendingChannels.isEmpty() || !state->outgoingMessageQueue.isEmpty()
        || !state->outgoingPacketsQueue.isEmpty())
        return fail("closed association retained channels or outgoing packets");
    if (reader->writeDatagram(QNetworkDatagram("late-write")) || state->newChannel(Reliable, true, 0, 256, {}, {}))
        return fail("closed association accepted new data or a new channel");

    state->OnSctpAssociationClosed(&state->assoc);
    state->onTransportClosed();
    QCoreApplication::processEvents();
    if (pendingClosed != 1 || writerClosed != 1 || readerDisconnected != 1 || readerClosed)
        return fail("duplicate terminal notifications closed a channel twice");
    state.reset();
    if (reader->association || writer->association || pending->association)
        return fail("closed channels retained a destroyed association");
    if (reader->readDatagram().data() != payload || readerClosed)
        return fail("closure overtook delivery of the final buffered datagram");
    QCoreApplication::processEvents();
    if (readerClosed != 1 || reader->isOpen())
        return fail("reader did not finish closure after draining buffered data");
    return 0;
}

int checkReentrantDestruction()
{
    Association owner(nullptr);
    auto        state   = std::make_unique<AssociationPrivate>(&owner);
    auto        one     = state->newChannel(Reliable, true, 0, 256, {}, {});
    auto        two     = state->newChannel(Reliable, true, 0, 256, {}, {});
    int         closed  = 0;
    const auto  destroy = [&] {
        ++closed;
        state.reset();
    };
    QObject::connect(one.data(), &ByteStream::connectionClosed, &owner, destroy);
    QObject::connect(two.data(), &ByteStream::connectionClosed, &owner, destroy);
    state->OnSctpAssociationClosed(&state->assoc);
    if (!state || closed)
        return fail("observer destroyed the association inside an SCTP callback");
    QCoreApplication::processEvents();
    if (state || closed != 2)
        return fail("first observer destruction prevented sibling closure");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    for (const auto closure :
         { Closure::SctpClosed, Closure::SctpFailed, Closure::TransportClosed, Closure::TransportError }) {
        if (const auto result = checkClosure(closure))
            return result;
    }
    return checkReentrantDestruction();
}
