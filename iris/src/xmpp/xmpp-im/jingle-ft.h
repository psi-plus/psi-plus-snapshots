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

#include "jingle.h"
#include "xmpp_hash.h"

namespace XMPP {

class Client;
class Thumbnail;

namespace Jingle {
namespace FileTransfer {

extern const QString NS;
class Manager;

struct Range {
    quint64 offset = 0;
    quint64 length = 0; // 0 - from offset to the end of the file
    Hash hash;

    inline Range() {}
    inline Range(quint64 offset, quint64 length) : offset(offset), length(length) {}
    inline bool isValid() const { return hash.isValid() || offset || length; }
    QDomElement toXml(QDomDocument *doc) const;
};

class File
{
public:
    struct Histogram
    {
        enum Coding {
            U8,  S8,
            U16, S16,
            U32, S32
        };

        Coding coding;
        QList<quint32> bars;
    };

    File();
    File(const File &other);
    File(const QDomElement &file);
    ~File();
    File& operator=(const File& other);
    inline bool isValid() const { return d != nullptr; }
    QDomElement toXml(QDomDocument *doc) const;
    bool merge(const File &other);
    bool hasComputedHashes() const;
    bool hasSize() const;

    QDateTime date()  const;
    QString   description() const;
    Hash      hash(Hash::Type t = Hash::Unknown)  const;
    QString   mediaType() const;
    QString   name()  const;
    quint64   size()  const;
    Range     range() const;
    Thumbnail thumbnail() const;
    Histogram  audioHistogram() const;
    template<typename T>
    QList<T> audioHistogram() const
    {
        QList<T> ret;
        auto hg = audioHistogram();
        std::function<quint8(quint32)> normalizer;
        switch (hg.coding) {
        case Histogram::U8:  normalizer = [](quint32 v){ return v/T(255);             }; break;
        case Histogram::S8:  normalizer = [](quint32 v){ return std::abs(v/T(128));   }; break;
        case Histogram::U16: normalizer = [](quint32 v){ return v/T(2<<16);           }; break;
        case Histogram::S16: normalizer = [](quint32 v){ return std::abs(v/T(2<<15)); }; break;
        case Histogram::U32: normalizer = [](quint32 v){ return quint64(v)/T(2L<<32);          }; break;
        case Histogram::S32: normalizer = [](quint32 v){ return std::abs(qint64(v)/T(2L<<31)); }; break;
        }
        if (normalizer)
            std::transform(hg.bars.begin(), hg.bars.end(), std::back_inserter(ret), normalizer);
        return ret;
    }

    void setDate(const QDateTime &date);
    void setDescription(const QString &desc);
    void addHash(const Hash &hash);
    void setMediaType(const QString &mediaType);
    void setName(const QString &name);
    void setSize(quint64 size);
    void setRange(const Range &range = Range()); // default empty just to indicate it's supported
    void setThumbnail(const Thumbnail &thumb);
    void setAudioHistogram(const Histogram &histogram);
private:
    class Private;
    Private *ensureD();
    QSharedDataPointer<Private> d;
};

class Checksum : public ContentBase
{
    inline Checksum(){}
    Checksum(const QDomElement &file);
    bool isValid() const;
    QDomElement toXml(QDomDocument *doc) const;
private:
    File file;
};

class Received : public ContentBase
{
    using ContentBase::ContentBase;
    QDomElement toXml(QDomDocument *doc) const;
};

class Pad : public ApplicationManagerPad
{
    Q_OBJECT
    // TODO
public:
    Pad(Manager *manager, Session *session);
    QDomElement takeOutgoingSessionInfoUpdate() override;
    QString ns() const override;
    Session *session() const override;
    ApplicationManager *manager() const override;
    QString generateContentName(Origin senders) override;

    void addOutgoingOffer(const File &file);
private:
    Manager *_manager;
    Session *_session;
};

class Application : public XMPP::Jingle::Application
{
    Q_OBJECT
public:
    Application(const QSharedPointer<Pad> &pad, const QString &contentName, Origin creator, Origin senders);
    ~Application();

    ApplicationManagerPad::Ptr pad() const override;
    State state() const override;
    void setState(State state) override;
    XMPP::Stanza::Error lastError() const override;
    Reason terminationReason() const override;

    QString contentName() const override;
    Origin creator() const override;
    Origin senders() const override;
    Origin transportReplaceOrigin() const override;
    SetDescError setDescription(const QDomElement &description) override;
    void setFile(const File &file);
    File file() const;
    File acceptFile() const;
    bool setTransport(const QSharedPointer<Transport> &transport) override;
    QSharedPointer<Transport> transport() const override;

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

    Action evaluateOutgoingUpdate() override;
    OutgoingUpdate takeOutgoingUpdate() override;
    bool wantBetterTransport(const QSharedPointer<XMPP::Jingle::Transport> &) const override;
    bool selectNextTransport() override;
    void prepare() override;
    void start() override;
    bool accept(const QDomElement &el) override;
    void remove(Reason::Condition cond = Reason::Success, const QString &comment = QString()) override;

    bool isValid() const;

    void setDevice(QIODevice *dev, bool closeOnFinish = true);
    Connection::Ptr connection() const;

protected:
    bool incomingTransportReplace(const QSharedPointer<Transport> &transport) override;
    bool incomingTransportAccept(const QDomElement &transportEl) override;

signals:
    void connectionReady(); // streaming mode only
    void deviceRequested(quint64 offset, quint64 size); // if size = 0 then it's reamaining part of the file (non-streaming mode only)
    void progress(quint64 offset);

private:
    class Private;
    QScopedPointer<Private> d;
};

class Manager : public XMPP::Jingle::ApplicationManager
{
    Q_OBJECT
public:
    Manager(QObject *parent = nullptr);
    ~Manager();
    void setJingleManager(XMPP::Jingle::Manager *jm);
    Application *startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName, Origin creator, Origin senders);
    ApplicationManagerPad *pad(Session *session); // pad factory
    void closeAll();
    Client* client();

    QStringList availableTransports() const;
private:
    XMPP::Jingle::Manager *jingleManager = nullptr;
};

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP

#endif // JINGLEFT_H
