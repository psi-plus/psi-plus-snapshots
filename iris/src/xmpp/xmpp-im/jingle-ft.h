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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef JINGLEFT_H
#define JINGLEFT_H

#include "jingle.h"

namespace XMPP {

class Client;
class Thumbnail;

namespace Jingle {
namespace FileTransfer {

extern const QString NS;
class Manager;

struct Range {
    quint64 offset = 0;
    quint64 length = 0;
    Hash hash;

    inline bool isValid() const { return hash.isValid() || offset || length; }
    QDomElement toXml(QDomDocument *doc) const;
};

class File
{
public:
    File();
    File(const File &other);
    File(const QDomElement &file);
    ~File();
    inline bool isValid() const { return d != nullptr; }
    QDomElement toXml(QDomDocument *doc) const;

    QDateTime date()  const;
    QString description() const;
    Hash    hash()  const;
    QString mediaType() const;
    QString name()  const;
    quint64 size()   const;
    Range     range() const;
    Thumbnail thumbnail() const;

    void setDate(const QDateTime &date);
    void setDescription(const QString &desc);
    void setHash(const Hash &hash);
    void setMediaType(const QString &mediaType);
    void setName(const QString &name);
    void setSize(quint64 size);
    void setRange(const Range &range = Range()); // default empty just to indicate it's supported
    void setThumbnail(const Thumbnail &thumb);
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
    Manager *manager() const;

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

    QString contentName() const;
    SetDescError setDescription(const QDomElement &description) override;
    bool setTransport(const QSharedPointer<Transport> &transport) override;
    QSharedPointer<Transport> transport() const override;

    Jingle::Action outgoingUpdateType() const override;
    bool isReadyForSessionAccept() const override;
    QDomElement takeOutgoingUpdate() override;
    QDomElement sessionAcceptContent() const override;
    bool wantBetterTransport(const QSharedPointer<XMPP::Jingle::Transport> &) const override;

    bool isValid() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

class Manager : public XMPP::Jingle::ApplicationManager
{
    Q_OBJECT
public:
    Manager(QObject *parent = nullptr);
    void setJingleManager(XMPP::Jingle::Manager *jm);
    Application *startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName, Origin creator, Origin senders);
    ApplicationManagerPad *pad(Session *session); // pad factory
    void closeAll();
    Client* client();

private:
    XMPP::Jingle::Manager *jingleManager = nullptr;
};

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP

#endif // JINGLEFT_H
