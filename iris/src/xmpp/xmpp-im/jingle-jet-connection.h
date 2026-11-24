/*
 * jingle-jet-connection.h - authenticated Jingle byte-stream wrapper
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef JINGLE_JET_CONNECTION_H
#define JINGLE_JET_CONNECTION_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-connection.h>

#include <QQueue>
#include <memory>

namespace XMPP { namespace Jingle { namespace JET {
    class StreamCipher;

    // Presents plaintext through the usual Jingle::Connection interface.
    // One AES-GCM record covers exactly plainSize bytes, followed by a
    // 16-byte authentication tag. This is not a framed indefinite stream.
    class IRIS_EXPORT Connection final : public XMPP::Jingle::Connection {
        Q_OBJECT
    public:
        Connection(XMPP::Jingle::Connection::Ptr raw, const QByteArray &key, const QByteArray &iv, bool sender,
                   quint64 plainSize);
        ~Connection() override;

        bool isValid() const { return bool(cipher_) && !failed_; }
        bool authenticated() const { return sender_ || verified_; }
        bool finished() const { return finished_; }
        quint64 plainSize() const { return plainSize_; }

        TransportFeatures features() const override;
        size_t blockSize() const override;
        int component() const override;
        bool hasPendingDatagrams() const override;
        QNetworkDatagram readDatagram(qint64 maxSize = -1) override;
        bool writeDatagram(const QNetworkDatagram &datagram) override;
        qint64 bytesAvailable() const override;
        qint64 bytesToWrite() const override;
        void close() override;

    protected:
        qint64 readDataInternal(char *data, qint64 maxSize) override;
        qint64 writeData(const char *data, qint64 maxSize) override;

    private:
        void pump();
        void fail(const QString &message);
        bool writeWire(const QByteArray &data);
        void finishSending();

        XMPP::Jingle::Connection::Ptr raw_;
        std::unique_ptr<StreamCipher> cipher_;
        quint64 plainSize_ = 0;
        quint64 plainWritten_ = 0;
        quint64 plainAcknowledged_ = 0;
        quint64 wireRead_ = 0;
        bool sender_ = false;
        bool finished_ = false;
        bool verified_ = false;
        bool failed_ = false;
        QQueue<int> pendingFrameSizes_;
        QQueue<QByteArray> plainFrames_;
        QByteArray pendingPlain_;
        qint64 availableFrames_ = 0;
    };
}}} // namespace XMPP::Jingle::JET

#endif // JINGLE_JET_CONNECTION_H
