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

#include "jingle-application.h"
#include "jingle-file.h"
#include "jingle.h"
#include "xmpp_hash.h"

namespace XMPP {
class Client;
class Thumbnail;
}

namespace XMPP { namespace Jingle { namespace FileTransfer {

    extern const QString NS;
    class Manager;

    class Pad : public ApplicationManagerPad {
        Q_OBJECT
        // TODO
    public:
        Pad(Manager *manager, Session *session);
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

    class Application : public XMPP::Jingle::Application {
        Q_OBJECT
    public:
        Application(const QSharedPointer<Pad> &pad, const QString &contentName, Origin creator, Origin senders);
        ~Application() override;

        bool                isValid() const;
        void                setState(State state) override;
        XMPP::Stanza::Error lastError() const override;
        Reason              lastReason() const override;

        bool isTransportReplaceEnabled() const override;
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
        void         setJingleManager(XMPP::Jingle::Manager *jm) override;
        Application *startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName, Origin creator,
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
