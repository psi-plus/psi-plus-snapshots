// SPDX-License-Identifier: LGPL-2.1-or-later
// JET::Connection must be interchangeable with Jingle::Connection for
// known-length file streams, including message-oriented transports.

#include <iris/jingle-jet-connection.h>
#include <iris/jingle-jet.h>
#include "../../src/xmpp/xmpp-im/jingle-jet_p.h"

#include <QCoreApplication>
#include <QQueue>
#include <QtCrypto>

#include <algorithm>
#include <cstring>

namespace J = XMPP::Jingle;

namespace {
void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class MemoryConnection final : public J::Connection {
public:
    explicit MemoryConnection(bool messageOriented) : messageOriented_(messageOriented)
    {
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    void pairWith(MemoryConnection *peer) { peer_ = peer; }
    void corruptFinalTag(bool enabled) { corruptFinalTag_ = enabled; }
    void dropFinalTag(bool enabled) { dropFinalTag_ = enabled; }
    quint64 wireWritten() const { return wireWritten_; }

    J::TransportFeatures features() const override
    {
        auto flags = J::TransportFeature::Reliable | J::TransportFeature::Ordered
            | J::TransportFeature::DataOriented;
        if (messageOriented_)
            flags |= J::TransportFeature::MessageOriented;
        return flags;
    }

    size_t blockSize() const override { return 7; }
    bool hasPendingDatagrams() const override { return !packets_.isEmpty(); }
    QNetworkDatagram readDatagram(qint64) override
    {
        if (packets_.isEmpty())
            return {};
        return QNetworkDatagram(packets_.dequeue());
    }
    bool writeDatagram(const QNetworkDatagram &datagram) override
    {
        return writeData(datagram.data().constData(), datagram.data().size()) == datagram.data().size();
    }

    qint64 bytesAvailable() const override
    {
        return messageOriented_ ? packets_.size() : ByteStream::bytesAvailable();
    }

protected:
    qint64 readDataInternal(char *data, qint64 maxSize) override
    {
        return ByteStream::readData(data, maxSize);
    }

    qint64 writeData(const char *data, qint64 size) override
    {
        if (!peer_ || size < 0)
            return -1;
        QByteArray bytes(data, int(size));
        wireWritten_ += quint64(size);
        if (size == 16 && (corruptFinalTag_ || dropFinalTag_)) {
            if (dropFinalTag_)
                return size;
            bytes[7] = char(bytes[7] ^ 0x42);
        }
        if (messageOriented_)
            peer_->packets_.enqueue(bytes);
        else
            peer_->appendRead(bytes);
        emit peer_->readyRead();
        emit bytesWritten(size);
        return size;
    }

private:
    MemoryConnection *peer_ = nullptr;
    bool messageOriented_ = false;
    bool corruptFinalTag_ = false;
    bool dropFinalTag_ = false;
    quint64 wireWritten_ = 0;
    QQueue<QByteArray> packets_;
};

void exercise(bool packetMode, bool tampered, bool empty)
{
    auto rawSender = QSharedPointer<MemoryConnection>::create(packetMode);
    auto rawReceiver = QSharedPointer<MemoryConnection>::create(packetMode);
    rawSender->pairWith(rawReceiver.data());
    rawReceiver->pairWith(rawSender.data());
    rawSender->corruptFinalTag(tampered);

    const QByteArray payload = empty ? QByteArray()
                                     : QByteArray("a short message split into seven-byte chunks");
    const QByteArray key = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray iv = QByteArray::fromHex("000102030405060708090a0b");

    // A receiver may subscribe before or after ciphertext has arrived.
    auto receiver = QSharedPointer<J::JET::Connection>::create(rawReceiver, key, iv, false, payload.size());
    check(receiver->isValid(), "JET receiving wrapper could not be created");
    int errors = 0;
    QObject::connect(receiver.data(), &ByteStream::error, receiver.data(), [&errors](int) { ++errors; });
    auto sender = QSharedPointer<J::JET::Connection>::create(rawSender, key, iv, true, payload.size());
    check(sender->isValid(), "JET sending wrapper could not be created");

    for (int pos = 0; pos < payload.size(); pos += 7) {
        const auto part = payload.mid(pos, 7);
        if (packetMode)
            check(sender->writeDatagram(QNetworkDatagram(part)), "JET datagram write failed");
        else
            check(sender->write(part) == part.size(), "JET stream write failed");
    }

    QByteArray received;
    while (receiver->bytesAvailable() > 0 || receiver->hasPendingDatagrams()) {
        if (packetMode)
            received += receiver->readDatagram().data();
        else
            received += receiver->readAll();
    }

    check(rawSender->wireWritten() == quint64(payload.size() + 16), "JET ciphertext length is wrong");
    check(sender->finished(), "JET sender did not emit the authentication tag");

    if (tampered) {
        check(errors > 0 && !receiver->authenticated(), "JET accepted a corrupt GCM tag");
        check(received != payload, "JET delivered a forged final payload as authenticated");
    } else {
        check(errors == 0 && receiver->authenticated(), "JET did not authenticate the ciphertext");
        check(received == payload, "JET plaintext differs from the original");
    }
}

void missingTag()
{
    auto rawSender = QSharedPointer<MemoryConnection>::create(false);
    auto rawReceiver = QSharedPointer<MemoryConnection>::create(false);
    rawSender->pairWith(rawReceiver.data());
    rawReceiver->pairWith(rawSender.data());
    rawSender->dropFinalTag(true);
    const QByteArray key(16, char(0x12));
    const QByteArray iv(12, char(0x34));
    auto receiver = QSharedPointer<J::JET::Connection>::create(rawReceiver, key, iv, false, 4);
    auto sender = QSharedPointer<J::JET::Connection>::create(rawSender, key, iv, true, 4);
    int failures = 0;
    QObject::connect(receiver.data(), &ByteStream::error, receiver.data(), [&failures](int) { ++failures; });
    check(sender->write(QByteArray("test")) == 4, "Could not send incomplete JET ciphertext");
    emit rawReceiver->connectionClosed();
    check(failures > 0 && !receiver->authenticated(), "JET accepted an incomplete authentication tag");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    if (!J::JET::StreamCipher::isSupported(16)) {
        qInfo("JET connection tests skipped: AES-GCM/CTR unavailable");
        return 0;
    }
    exercise(false, false, false);
    exercise(true, false, false);
    exercise(false, true, false);
    exercise(true, true, false);
    exercise(false, false, true);
    exercise(true, false, true);
    missingTag();
    return 0;
}
