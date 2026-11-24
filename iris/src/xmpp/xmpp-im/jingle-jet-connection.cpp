/*
 * jingle-jet-connection.cpp - authenticated Jingle byte-stream wrapper
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "jingle-jet-connection.h"

#include "jingle-jet_p.h"

#include <QPointer>

#include <algorithm>
#include <cstring>
#include <limits>

namespace XMPP { namespace Jingle { namespace JET {
    Connection::Connection(XMPP::Jingle::Connection::Ptr raw, const QByteArray &key, const QByteArray &iv, bool sender,
                           quint64 plainSize) :
        raw_(std::move(raw)), plainSize_(plainSize), sender_(sender)
    {
        if (!raw_ || plainSize_ > std::numeric_limits<quint64>::max() - StreamCipher::TagSize)
            return;
        cipher_ = sender_ ? StreamCipher::encryptor(key, iv) : StreamCipher::decryptor(key, iv);
        if (!cipher_)
            return;

        setRemote(raw_->isRemote());
        open(QIODevice::ReadWrite | QIODevice::Unbuffered);

        connect(raw_.data(), &QIODevice::readyRead, this, &Connection::pump);
        connect(raw_.data(), &QIODevice::bytesWritten, this, [this](qint64 count) {
            if (failed_ || count <= 0)
                return;
            const auto acknowledged = qMin(quint64(count), plainWritten_ - plainAcknowledged_);
            plainAcknowledged_ += acknowledged;
            if (acknowledged)
                emit bytesWritten(qint64(acknowledged));
        });
        connect(raw_.data(), &XMPP::Jingle::Connection::connected, this, &Connection::connected);
        connect(raw_.data(), &XMPP::Jingle::Connection::disconnected, this, &Connection::disconnected);
        connect(raw_.data(), &ByteStream::connectionClosed, this, [this]() {
            QPointer<Connection> guard(this);
            if (!sender_ && !verified_ && !failed_)
                fail(QStringLiteral("JET stream closed before authentication completed"));
            if (guard)
                emit connectionClosed();
        });
        connect(raw_.data(), &ByteStream::delayedCloseFinished, this, &Connection::delayedCloseFinished);
        connect(raw_.data(), &ByteStream::error, this, [this](int code) {
            if (!failed_)
                setError(code, raw_->errorText());
        });

        // A zero-byte file still has an authentication tag on the wire.
        if (sender_ && !plainSize_)
            finishSending();
        if (!sender_)
            pump();
    }

    Connection::~Connection() = default;

    TransportFeatures Connection::features() const { return raw_ ? raw_->features() : TransportFeatures(); }
    size_t Connection::blockSize() const { return raw_ ? raw_->blockSize() : 0; }
    int Connection::component() const { return raw_ ? raw_->component() : 0; }
    qint64 Connection::bytesAvailable() const
    {
        return bool(features() & TransportFeature::MessageOriented) ? availableFrames_ : ByteStream::bytesAvailable();
    }
    qint64 Connection::bytesToWrite() const { return raw_ ? raw_->bytesToWrite() : 0; }

    bool Connection::hasPendingDatagrams() const
    {
        return bool(features() & TransportFeature::MessageOriented) && !plainFrames_.isEmpty();
    }

    QNetworkDatagram Connection::readDatagram(qint64 maxSize)
    {
        if (!hasPendingDatagrams())
            return {};
        auto frame = plainFrames_.dequeue();
        availableFrames_ -= frame.size();
        if (maxSize >= 0 && maxSize < frame.size())
            frame.truncate(int(maxSize));
        return QNetworkDatagram(frame);
    }

    bool Connection::writeDatagram(const QNetworkDatagram &datagram)
    {
        return write(datagram.data()) == datagram.data().size();
    }

    qint64 Connection::readDataInternal(char *data, qint64 maxSize)
    {
        if (!(features() & TransportFeature::MessageOriented))
            return ByteStream::readData(data, maxSize);
        qint64 count = 0;
        while (count < maxSize && !plainFrames_.isEmpty()) {
            auto &front = plainFrames_.head();
            const int size = int(qMin(qint64(front.size()), maxSize - count));
            std::memcpy(data + count, front.constData(), size_t(size));
            front.remove(0, size);
            count += size;
            availableFrames_ -= size;
            if (front.isEmpty())
                plainFrames_.dequeue();
        }
        return count;
    }

    bool Connection::writeWire(const QByteArray &data)
    {
        if (!raw_)
            return false;
        if (data.isEmpty())
            return true;
        if (raw_->features() & TransportFeature::MessageOriented)
            return raw_->writeDatagram(QNetworkDatagram(data));
        return raw_->write(data) == data.size();
    }

    qint64 Connection::writeData(const char *data, qint64 maxSize)
    {
        if (!sender_ || !cipher_ || failed_ || finished_ || maxSize < 0
            || quint64(maxSize) > plainSize_ - plainWritten_
            || maxSize > std::numeric_limits<int>::max()) {
            fail(QStringLiteral("JET write exceeds the negotiated plaintext length"));
            return -1;
        }
        if (!maxSize)
            return 0;
        const auto encrypted = cipher_->process(QByteArray(data, int(maxSize)));
        plainWritten_ += quint64(maxSize);
        QPointer<Connection> guard(this);
        if (encrypted.size() != maxSize || !writeWire(encrypted)) {
            if (guard)
                fail(QStringLiteral("Failed writing encrypted JET data"));
            return -1;
        }
        if (!guard)
            return -1;
        if (plainWritten_ == plainSize_)
            finishSending();
        return guard && !failed_ ? maxSize : -1;
    }

    void Connection::finishSending()
    {
        if (failed_ || finished_ || !cipher_)
            return;
        const auto tag = cipher_->finish();
        QPointer<Connection> guard(this);
        if (tag.size() != StreamCipher::TagSize || !writeWire(tag)) {
            if (guard)
                fail(QStringLiteral("Failed to finish the authenticated JET stream"));
            return;
        }
        if (guard)
            finished_ = true;
    }

    void Connection::pump()
    {
        if (sender_ || !cipher_ || failed_ || !raw_)
            return;
        const auto expected = plainSize_ + quint64(StreamCipher::TagSize);
        if (wireRead_ == expected) {
            if (raw_->bytesAvailable() > 0 || raw_->hasPendingDatagrams())
                fail(QStringLiteral("Unexpected data after JET authentication tag"));
            return;
        }
        while (!failed_ && wireRead_ < expected
               && (raw_->bytesAvailable() > 0 || raw_->hasPendingDatagrams())) {
            QByteArray chunk;
            if (raw_->features() & TransportFeature::MessageOriented)
                chunk = raw_->readDatagram().data();
            else
                chunk = raw_->read(qint64(qMin(quint64(65536), expected - wireRead_)));
            if (chunk.isEmpty())
                break;
            if (quint64(chunk.size()) > expected - wireRead_) {
                fail(QStringLiteral("JET payload exceeds its negotiated length"));
                return;
            }
            const bool messageOriented = bool(raw_->features() & TransportFeature::MessageOriented);
            if (messageOriented && wireRead_ < plainSize_)
                pendingFrameSizes_.enqueue(chunk.size());
            wireRead_ += quint64(chunk.size());
            auto plain = cipher_->process(chunk);
            if (wireRead_ == expected) {
                if (!cipher_->verify()) {
                    fail(QStringLiteral("JET authentication tag is invalid"));
                    return;
                }
                verified_ = true;
                finished_ = true;
            }
            if (!plain.isEmpty()) {
                if (messageOriented) {
                    pendingPlain_ += plain;
                    while (!pendingFrameSizes_.isEmpty()
                           && pendingPlain_.size() >= pendingFrameSizes_.head()) {
                        const auto size = pendingFrameSizes_.dequeue();
                        plainFrames_.enqueue(pendingPlain_.left(size));
                        pendingPlain_.remove(0, size);
                        availableFrames_ += size;
                    }
                } else {
                    appendRead(plain);
                }
                if (!messageOriented || hasPendingDatagrams()) {
                    QPointer<Connection> guard(this);
                    emit readyRead();
                    if (!guard)
                        return;
                }
            }
            if (finished_ && messageOriented && (!pendingFrameSizes_.isEmpty() || !pendingPlain_.isEmpty())) {
                fail(QStringLiteral("JET datagram framing is incomplete"));
                return;
            }
            if (finished_ && plain.isEmpty()) {
                // A zero-length file still needs to notify its reader that
                // the authentication tag has arrived.
                QPointer<Connection> guard(this);
                emit readyRead();
                if (!guard)
                    return;
            }
        }
    }

    void Connection::fail(const QString &message)
    {
        if (failed_)
            return;
        failed_ = true;
        setError(sender_ ? ByteStream::ErrWrite : ByteStream::ErrRead, message);
    }

    void Connection::close()
    {
        QPointer<Connection> guard(this);
        if (raw_)
            raw_->close();
        if (guard)
            QIODevice::close();
    }
}}} // namespace XMPP::Jingle::JET
