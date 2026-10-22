/*
 * jignle-ft.h - Jingle file transfer
 * Copyright (C) 2019-2024  Sergey Ilinykh
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

#ifndef JINGLEFT_H
#define JINGLEFT_H

// Feature marker for clients that support explicit range-receipt handover.
#define IRIS_FT_DEFERRED_RECEIPTS 1

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-application.h>
#include <iris/xmpp-im/jingle-file.h>
#include <iris/xmpp-im/jingle.h>
#include <iris/xmpp-im/xmpp_hash.h>

namespace XMPP {
class Client;
class Thumbnail;
}

namespace XMPP { namespace Jingle { namespace FileTransfer {

    extern IRIS_EXPORT const QString NS;
    class Manager;

    class Pad : public ApplicationManagerPad {
        Q_OBJECT
        // TODO
    public:
        Pad(Manager *manager, Session *session);
        ~Pad() override;
        QDomElement         takeOutgoingSessionInfoUpdate() override;
        QString             ns() const override;
        Session            *session() const override;
        ApplicationManager *manager() const override;
        QString             generateContentName(Origin senders) override;
        bool                incomingSessionInfo(const QDomElement &el) override;

        void addOutgoingOffer(const File &file);

    private:
        Manager *_manager;
        Session *_session;
    };

    class IRIS_EXPORT Application : public XMPP::Jingle::Application {
        Q_OBJECT
    public:
        Application(Pad *pad, const QString &contentName, Origin creator, Origin senders);
        ~Application() override;

        bool isValid() const;
        void setState(State state) override;

        const std::optional<XMPP::Stanza::Error> &lastError() const override;
        Reason                                    lastReason() const override;

        bool isTransportReplaceEnabled() const override;
        bool allowsSharedTransport() const override { return true; }
        void remove(Reason::Condition cond = Reason::Success, const QString &comment = QString()) override;

        void setFile(const File &file);
        void setFile(const QFileInfo &fi, const QString &description, const Thumbnail &thumb);
        File file() const;
        File acceptFile() const; // either local or remote File as an answer to the offer
        void setAcceptFile(const File &file) const;

        /**
         * @brief setStreamingMode enables external download control.
         *
         * When streaming mode is enabled:
         *   - `connectionReady()` signal to understand when to get ready to use connection
         *   - `Connection::Ptr connection()` to get connection
         * When streaming mode is disabled:
         *   - `deviceRequested(quint64 offset, optional<quint64> size)` signal to use `setDevice()`
         *   - `setDevice(QIODevice *dev, bool closeOnFinish)` to set input/output device
         *
         *  Make sure to set the mode before connection is established.
         * @param mode
         */
        void setStreamingMode(bool mode = true);

        // Opt in before starting the payload. Keep this application eligible for
        // BUNDLE extension while its completed payload awaits the receipt exchange.
        void setKeepTransportUntilReceipt(bool enabled = true);
        // Receivers may wait for a successor content-accept before confirming this
        // range. The integrity check still completes independently of the receipt.
        void setReceiptDeferred(bool enabled = true);
        bool acknowledgeReceived();
        // Pause only the receive pump; already received bytes retain their offsets.
        void setReceivingPaused(bool paused);

        /**
         * End-to-end encryption of the file data with JET (XEP-0391): the whole transfer is AES-GCM with
         * the 16-byte tag appended, as Conversations and Dino send it; size and hashes describe the plain
         * file. The key and the 12-byte IV come from an envelope that a signaling filter such as
         * JetOmemo exchanges. Set before the connection is established; not for ranged transfers.
         */
        void setTransportSecret(const QByteArray &key, const QByteArray &iv);
        bool isTransportEncrypted() const;

        void            setDevice(QIODevice *dev, bool closeOnFinish = true);
        Connection::Ptr connection() const;

        // next method are used by Jingle::Session and usually shouldn't be called manually
        XMPP::Jingle::Application::Update evaluateOutgoingUpdate() override;
        OutgoingUpdate                    takeOutgoingUpdate() override;
        void                              prepare() override;
        void                              start() override;
        SetDescError                      setRemoteOffer(const QDomElement &description) override;
        SetDescError                      setRemoteAnswer(const QDomElement &description) override;

    protected:
        QDomElement makeLocalOffer() override;
        QDomElement makeLocalAnswer() override;

        void incomingRemove(const Reason &r) override;
        void prepareTransport() override;

    signals:
        void connectionReady(); // streaming mode only
        void payloadVerified(); // receiver: bytes and negotiated checksum are valid
        void remoteChecksumReceived(const XMPP::Jingle::FileTransfer::File &file);

        // if size is not set then it's reamaining part of the file (non-streaming mode only)
        void deviceRequested(quint64 offset, std::optional<quint64> size);
        void progress(quint64 offset);

    private:
        friend class Pad;
        class Private;
        std::unique_ptr<Private> d;
    };

    class Manager : public XMPP::Jingle::ApplicationManager {
        Q_OBJECT
    public:
        Manager(QObject *parent = nullptr);
        ~Manager();
        void                   setJingleManager(XMPP::Jingle::Manager *jm) override;
        Application           *startApplication(ApplicationManagerPad *pad, const QString &contentName, Origin creator,
                                                Origin senders) override;
        ApplicationManagerPad *pad(Session *session) override; // pad factory
        void                   closeAll(const QString &ns = QString()) override;

        QStringList discoFeatures() const override;

        Client *client();

        QStringList availableTransports() const;

    private:
        XMPP::Jingle::Manager *jingleManager = nullptr;
    };

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP

#endif // JINGLEFT_H
