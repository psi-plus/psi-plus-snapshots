/*
 * jignle-ft.h - Jingle file transfer
 * Copyright (C) 2019  Sergey Ilinykh
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
#include "jingle.h"
#include "xmpp_hash.h"

namespace XMPP {
class Client;
class Thumbnail;
}

namespace XMPP { namespace Jingle { namespace FileTransfer {

    extern const QString NS;
    class Manager;

    struct Range {
        qint64      offset = 0;
        qint64      length = 0; // 0 - from offset to the end of the file
        QList<Hash> hashes;

        inline Range() {}
        inline Range(qint64 offset, qint64 length) : offset(offset), length(length) {}
        inline bool isValid() const { return hashes.size() || offset || length; }
        inline      operator bool() const { return isValid(); }
        QDomElement toXml(QDomDocument *doc) const;
    };

    class File {
    public:
        File();
        File(const File &other);
        File(const QDomElement &file);
        ~File();
        File &      operator=(const File &other);
        inline bool isValid() const { return d != nullptr; }
        QDomElement toXml(QDomDocument *doc) const;
        bool        merge(const File &other);
        bool        hasComputedHashes() const;
        bool        hasSize() const;

        QDateTime   date() const;
        QString     description() const;
        QList<Hash> hashes() const;
        QList<Hash> computedHashes() const;
        Hash        hash(Hash::Type t = Hash::Unknown) const;
        QString     mediaType() const;
        QString     name() const;
        qint64      size() const;
        Range       range() const;
        Thumbnail   thumbnail() const;
        QByteArray  amplitudes() const;

        void setDate(const QDateTime &date);
        void setDescription(const QString &desc);
        void addHash(const Hash &hash);
        void setHashes(const QList<Hash> &hashes);
        void setMediaType(const QString &mediaType);
        void setName(const QString &name);
        void setSize(qint64 size);
        void setRange(const Range &range = Range()); // default empty just to indicate it's supported
        void setThumbnail(const Thumbnail &thumb);
        void setAmplitudes(const QByteArray &amplitudes);

    private:
        class Private;
        Private *                   ensureD();
        QSharedDataPointer<Private> d;
    };

    class Checksum : public ContentBase {
        inline Checksum() {}
        Checksum(const QDomElement &file);
        bool        isValid() const;
        QDomElement toXml(QDomDocument *doc) const;

    private:
        File file;
    };

    class Received : public ContentBase {
        using ContentBase::ContentBase;
        QDomElement toXml(QDomDocument *doc) const;
    };

    class Pad : public ApplicationManagerPad {
        Q_OBJECT
        // TODO
    public:
        Pad(Manager *manager, Session *session);
        QDomElement         takeOutgoingSessionInfoUpdate() override;
        QString             ns() const override;
        Session *           session() const override;
        ApplicationManager *manager() const override;
        QString             generateContentName(Origin senders) override;

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

        void                setState(State state) override;
        XMPP::Stanza::Error lastError() const override;
        Reason              lastReason() const override;

        SetDescError setRemoteOffer(const QDomElement &description) override;
        SetDescError setRemoteAnswer(const QDomElement &description) override;
        QDomElement  makeLocalOffer() override;
        QDomElement  makeLocalAnswer() override;

        bool isTransportReplaceEnabled() const override;
        void remove(Reason::Condition cond = Reason::Success, const QString &comment = QString()) override;

        XMPP::Jingle::Application::Update evaluateOutgoingUpdate() override;
        OutgoingUpdate                    takeOutgoingUpdate() override;
        void                              prepare() override;
        void                              start() override;

        void setFile(const File &file);
        File file() const;
        File acceptFile() const;

        /**
         * @brief setStreamingMode enables external download control.
         *  So Jingle-FT won't request output device but instead underlying established
         *  connection will be emitted (see connectionReady() signal).
         *  The connection is an XMPP::Jingle::Connection::Ptr instance.
         *  When the connection is not needed anymore, one can just destroy jingle
         *  session or remove the Application from the session.
         *  Make sure to set the mode before connection is established.
         * @param mode
         */
        void setStreamingMode(bool mode = true);
        bool isValid() const;

        void            setDevice(QIODevice *dev, bool closeOnFinish = true);
        Connection::Ptr connection() const;

    protected:
        void incomingRemove(const Reason &r) override;
        void initTransport() override;

    private:
        void prepareThumbnail(File &file);

    signals:
        void connectionReady(); // streaming mode only

        // if size = 0 then it's reamaining part of the file (non-streaming mode only)
        void deviceRequested(qint64 offset, qint64 size);
        void progress(qint64 offset);

    private:
        class Private;
        QScopedPointer<Private> d;
    };

    class Manager : public XMPP::Jingle::ApplicationManager {
        Q_OBJECT
    public:
        Manager(QObject *parent = nullptr);
        ~Manager();
        void         setJingleManager(XMPP::Jingle::Manager *jm);
        Application *startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName, Origin creator,
                                      Origin senders);
        ApplicationManagerPad *pad(Session *session); // pad factory
        void                   closeAll();
        Client *               client();

        QStringList availableTransports() const;

    private:
        XMPP::Jingle::Manager *jingleManager = nullptr;
    };

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP

#endif // JINGLEFT_H
