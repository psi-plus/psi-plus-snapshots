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

namespace Jingle {
namespace FileTransfer {

extern const QString NS;

struct Range {
    quint64 offset = 0;
    quint64 length = 0;
    Hash hash;
};

class File
{
public:
    File();
    ~File();
    File(const File &other);
    File(const QDomElement &file);
    inline bool isValid() const { return d != nullptr; }
    QDomElement toXml(QDomDocument *doc) const;
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

class FTApplication : public Application
{
    Q_OBJECT
public:
    FTApplication(Client *client);
    void incomingSession(Session *session);
    QSharedPointer<Description> descriptionFromXml(const QDomElement &el);

private:
    Client *client;
};

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP

#endif // JINGLEFT_H
