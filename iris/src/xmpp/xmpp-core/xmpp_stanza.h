/*
 * Copyright (C) 2003  Justin Karneges
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

#ifndef XMPP_STANZA_H
#define XMPP_STANZA_H

#include <QDomElement>
#include <QPair>
#include <QSharedPointer>
#include <QString>

class QDomDocument;

namespace XMPP {
class Jid;
class Stream;

class Stanza {
public:
    enum Kind { Message, Presence, IQ };

    Stanza();
    Stanza(const Stanza &from);
    Stanza &operator=(const Stanza &from);
    virtual ~Stanza();

    class Error {
    public:
        enum class ErrorType { Cancel = 1, Continue, Modify, Auth, Wait };
        enum class ErrorCond {
            BadRequest = 1,
            Conflict,
            FeatureNotImplemented,
            Forbidden,
            Gone,
            InternalServerError,
            ItemNotFound,
            JidMalformed,
            NotAcceptable,
            NotAllowed,
            NotAuthorized,
            PolicyViolation,
            RecipientUnavailable,
            Redirect,
            RegistrationRequired,
            RemoteServerNotFound,
            RemoteServerTimeout,
            ResourceConstraint,
            ServiceUnavailable,
            SubscriptionRequired,
            UndefinedCondition,
            UnexpectedRequest
        };

        Error(ErrorType type = ErrorType::Cancel, ErrorCond condition = ErrorCond::UndefinedCondition,
              const QString &text = QString(), const QDomElement &appSpec = QDomElement());

        ErrorType   type;
        ErrorCond   condition;
        QString     text;
        QString     by;
        QDomElement appSpec;

        int  code() const;
        bool fromCode(int code);

        inline bool isCancel() const { return type == ErrorType::Cancel; }
        inline bool isContinue() const { return type == ErrorType::Continue; }
        inline bool isModify() const { return type == ErrorType::Modify; }
        inline bool isAuth() const { return type == ErrorType::Auth; }
        inline bool isWait() const { return type == ErrorType::Wait; }

        QPair<QString, QString> description() const;
        QString                 toString() const;

        QDomElement toXml(QDomDocument &doc, const QString &baseNS) const;
        bool        fromXml(const QDomElement &e, const QString &baseNS);

    private:
        class Private;
        int originalCode;
    };

    bool isNull() const;

    QDomElement element() const;
    QString     toString() const;

    QDomDocument &doc() const;
    QString       baseNS() const;
    QDomElement   createElement(const QString &ns, const QString &tagName);
    QDomElement   createTextElement(const QString &ns, const QString &tagName, const QString &text);
    void          appendChild(const QDomElement &e);

    Kind        kind() const;
    static Kind kind(const QString &tagName);
    void        setKind(Kind k);

    Jid     to() const;
    Jid     from() const;
    QString id() const;
    QString type() const;
    QString lang() const;

    void setTo(const Jid &j);
    void setFrom(const Jid &j);
    void setId(const QString &id);
    void setType(const QString &type);
    void setLang(const QString &lang);

    Error error() const;
    void  setError(const Error &err);
    void  clearError();

    void markHandled();
    void setSMId(unsigned long id);

    QSharedPointer<QDomDocument> unboundDocument(QSharedPointer<QDomDocument>);

private:
    friend class Stream;
    Stanza(Stream *s, Kind k, const Jid &to, const QString &type, const QString &id);
    Stanza(Stream *s, const QDomElement &e);

    class Private;
    Private *d;
};
} // namespace XMPP

#endif // XMPP_STANZA_H
