/*
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

#ifndef JINGLE_H
#define JINGLE_H

#include <QSharedDataPointer>

class QDomElement;
class QDomDocument;

#define JINGLE_NS "urn:xmpp:jingle:1"
#define JINGLE_FT_NS "urn:xmpp:jingle:apps:file-transfer:5"

namespace XMPP {
namespace Jingle {

class Jingle
{
public:
    enum Action {
        NoAction, // non-standard, just a default
        ContentAccept,
        ContentAdd,
        ContentModify,
        ContentReject,
        ContentRemove,
        DescriptionInfo,
        SecurityInfo,
        SessionAccept,
        SessionInfo,
        SessionInitiate,
        SessionTerminate,
        TransportAccept,
        TransportInfo,
        TransportReject,
        TransportReplace
    };

    inline Jingle(){}
    Jingle(const QDomElement &e);
    QDomElement element(QDomDocument *doc) const;
private:
    class Private;
    QSharedDataPointer<Private> d;
    Jingle::Private *ensureD();
};

class Reason {
    class Private;
public:
    enum Condition
    {
        NoReason = 0, // non-standard, just a default
        AlternativeSession,
        Busy,
        Cancel,
        ConnectivityError,
        Decline,
        Expired,
        FailedApplication,
        FailedTransport,
        GeneralError,
        Gone,
        IncompatibleParameters,
        MediaError,
        SecurityError,
        Success,
        Timeout,
        UnsupportedApplications,
        UnsupportedTransports
    };

    inline Reason(){}
    Reason(const QDomElement &el);
    inline bool isValid() const { return d != nullptr; }
    Condition condition() const;
    void setCondition(Condition cond);
    QString text() const;
    void setText(const QString &text);

    QDomElement element(QDomDocument *doc) const;

private:
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

class Content
{
public:
    enum class Creator {
        Initiator,
        Responder
    };

    enum class Senders {
        None,
        Both,
        Initiator,
        Responder
    };

    inline Content(){}
    Content(const QDomElement &content);
    inline bool isValid() const { return d != nullptr; }
    QDomElement element(QDomDocument *doc) const;
private:
    class Private;
    Private *ensureD();
    QSharedDataPointer<Private> d;
};

class FileTransfer
{
public:
    struct Range {
        quint64 offset;
        quint64 length;
    };

    inline FileTransfer(){}
    FileTransfer(const QDomElement &file);
    inline bool isValid() const { return d != nullptr; }
    QDomElement element(QDomDocument *doc) const;
private:
    class Private;
    Private *ensureD();
    QSharedDataPointer<Private> d;
};

class Description
{
public:
    enum class Type {
        Unrecognized, // non-standard, just a default
        FileTransfer, // urn:xmpp:jingle:apps:file-transfer:5
    };
private:
    class Private;
    Private *ensureD();
    QSharedDataPointer<Private> d;
};

} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_H
