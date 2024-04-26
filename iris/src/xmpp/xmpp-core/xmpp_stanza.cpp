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

#include "xmpp_stanza.h"

#include "xmpp/jid/jid.h"
#include "xmpp_clientstream.h"
#include "xmpp_stream.h"

#include <QCoreApplication>
#include <QDebug>

#include <array>
#include <optional>

using namespace XMPP;

#define NS_STANZAS "urn:ietf:params:xml:ns:xmpp-stanzas"
#define NS_XML "http://www.w3.org/XML/1998/namespace"

//----------------------------------------------------------------------------
// Stanza::Error
//----------------------------------------------------------------------------

/**
    \class Stanza::Error
    \brief Represents stanza error

    Stanza error consists of error type and condition.
    In addition, it may contain a human readable description,
    and application specific element.

    One of the usages of this class is to easily generate error XML:

    \code
    QDomElement e = createIQ(client()->doc(), "error", jid, id);
    Error error(Stanza::Error::Auth, Stanza::Error::NotAuthorized);
    e.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
    \endcode

    This class implements XEP-0086, which means that it can read both
    old and new style error elements. Also, generated XML will contain
    both type/condition and code.
    Error text in output XML is always presented in XMPP-style only.

    All functions will always try to guess missing information based on mappings defined in the XEP.
*/

/**

    \enum Stanza::Error::ErrorType
    \brief Represents error type
*/

/**
    \enum Stanza::Error::ErrorCond
    \brief Represents error condition
*/

/**
    \brief Constructs new error
*/
Stanza::Error::Error(ErrorType _type, ErrorCond _condition, const QString &_text, const QDomElement &_appSpec)
{
    type         = _type;
    condition    = _condition;
    text         = _text;
    appSpec      = _appSpec;
    originalCode = 0;
}

class Stanza::Error::Private {
public:
    struct ErrorTypeEntry {
        QString   str;
        ErrorType type;
    };
    static std::array<ErrorTypeEntry, 5> errorTypeTable;

    struct ErrorCondEntry {
        QString   str;
        ErrorCond cond;
    };
    static std::array<ErrorCondEntry, 22> errorCondTable;

    struct ErrorCodeEntry {
        ErrorCond cond;
        ErrorType type;
        int       code;
    };
    static std::array<ErrorCodeEntry, 22> errorCodeTable;

    struct ErrorDescEntry {
        ErrorCond   cond;
        const char *name;
        const char *str;
    };
    static std::array<ErrorDescEntry, 22> errorDescriptions;

    static std::optional<ErrorType> stringToErrorType(const QString &s)
    {
        for (auto const &entry : errorTypeTable) {
            if (s == entry.str)
                return entry.type;
        }
        return {};
    }

    static QString errorTypeToString(ErrorType x)
    {
        for (auto const &entry : errorTypeTable) {
            if (x == entry.type)
                return entry.str;
        }
        return {};
    }

    static std::optional<ErrorCond> stringToErrorCond(const QString &s)
    {
        for (auto const &entry : errorCondTable) {
            if (s == entry.str)
                return entry.cond;
        }
        return {};
    }

    static QString errorCondToString(ErrorCond x)
    {
        for (auto const &entry : errorCondTable) {
            if (x == entry.cond)
                return entry.str;
        }
        return QString();
    }

    static int errorTypeCondToCode(ErrorType t, ErrorCond c)
    {
        Q_UNUSED(t);
        for (auto const &entry : errorCodeTable) {
            if (c == entry.cond)
                return entry.code;
        }
        return 0;
    }

    static std::optional<std::pair<ErrorType, ErrorCond>> errorCodeToTypeCond(int x)
    {
        for (auto const &entry : errorCodeTable) {
            if (x == entry.code)
                return std::make_pair(entry.type, entry.cond);
        }
        return {};
    }

    static QPair<QString, QString> errorCondToDesc(ErrorCond x)
    {
        for (auto const &entry : errorDescriptions) {
            if (x == entry.cond)
                return QPair<QString, QString>(QCoreApplication::translate("Stanza::Error::Private", entry.name),
                                               QCoreApplication::translate("Stanza::Error::Private", entry.str));
        }
        return QPair<QString, QString>();
    }
};

std::array<Stanza::Error::Private::ErrorTypeEntry, 5> Stanza::Error::Private::errorTypeTable {
    { { QStringLiteral("cancel"), ErrorType::Cancel },
      { QStringLiteral("continue"), ErrorType::Continue },
      { QStringLiteral("modify"), ErrorType::Modify },
      { QStringLiteral("auth"), ErrorType::Auth },
      { QStringLiteral("wait"), ErrorType::Wait } }
};

std::array<Stanza::Error::Private::ErrorCondEntry, 22> Stanza::Error::Private::errorCondTable { {
    { QStringLiteral("bad-request"), ErrorCond::BadRequest },
    { QStringLiteral("conflict"), ErrorCond::Conflict },
    { QStringLiteral("feature-not-implemented"), ErrorCond::FeatureNotImplemented },
    { QStringLiteral("forbidden"), ErrorCond::Forbidden },
    { QStringLiteral("gone"), ErrorCond::Gone },
    { QStringLiteral("internal-server-error"), ErrorCond::InternalServerError },
    { QStringLiteral("item-not-found"), ErrorCond::ItemNotFound },
    { QStringLiteral("jid-malformed"), ErrorCond::JidMalformed },
    { QStringLiteral("not-acceptable"), ErrorCond::NotAcceptable },
    { QStringLiteral("not-allowed"), ErrorCond::NotAllowed },
    { QStringLiteral("not-authorized"), ErrorCond::NotAuthorized },
    { QStringLiteral("policy-violation"), ErrorCond::PolicyViolation },
    { QStringLiteral("recipient-unavailable"), ErrorCond::RecipientUnavailable },
    { QStringLiteral("redirect"), ErrorCond::Redirect },
    { QStringLiteral("registration-required"), ErrorCond::RegistrationRequired },
    { QStringLiteral("remote-server-not-found"), ErrorCond::RemoteServerNotFound },
    { QStringLiteral("remote-server-timeout"), ErrorCond::RemoteServerTimeout },
    { QStringLiteral("resource-constraint"), ErrorCond::ResourceConstraint },
    { QStringLiteral("service-unavailable"), ErrorCond::ServiceUnavailable },
    { QStringLiteral("subscription-required"), ErrorCond::SubscriptionRequired },
    { QStringLiteral("undefined-condition"), ErrorCond::UndefinedCondition },
    { QStringLiteral("unexpected-request"), ErrorCond::UnexpectedRequest },
} };

std::array<Stanza::Error::Private::ErrorCodeEntry, 22> Stanza::Error::Private::errorCodeTable { {
    { ErrorCond::BadRequest, ErrorType::Modify, 400 },
    { ErrorCond::Conflict, ErrorType::Cancel, 409 },
    { ErrorCond::FeatureNotImplemented, ErrorType::Cancel, 501 },
    { ErrorCond::Forbidden, ErrorType::Auth, 403 },
    { ErrorCond::Gone, ErrorType::Modify, 302 }, // permanent
    { ErrorCond::InternalServerError, ErrorType::Wait, 500 },
    { ErrorCond::ItemNotFound, ErrorType::Cancel, 404 },
    { ErrorCond::JidMalformed, ErrorType::Modify, 400 },
    { ErrorCond::NotAcceptable, ErrorType::Modify, 406 },
    { ErrorCond::NotAllowed, ErrorType::Cancel, 405 },
    { ErrorCond::NotAuthorized, ErrorType::Auth, 401 },
    { ErrorCond::PolicyViolation, ErrorType::Modify, 402 }, // it can be Wait too according to rfc6120
    { ErrorCond::RecipientUnavailable, ErrorType::Wait, 404 },
    { ErrorCond::Redirect, ErrorType::Modify, 302 }, // temporary
    { ErrorCond::RegistrationRequired, ErrorType::Auth, 407 },
    { ErrorCond::RemoteServerNotFound, ErrorType::Cancel, 404 },
    { ErrorCond::RemoteServerTimeout, ErrorType::Wait, 504 },
    { ErrorCond::ResourceConstraint, ErrorType::Wait, 500 },
    { ErrorCond::ServiceUnavailable, ErrorType::Cancel, 503 },
    { ErrorCond::SubscriptionRequired, ErrorType::Auth, 407 },
    { ErrorCond::UndefinedCondition, ErrorType::Wait, 500 }, // Note: any type matches really
    { ErrorCond::UnexpectedRequest, ErrorType::Wait, 400 },
} };

std::array<Stanza::Error::Private::ErrorDescEntry, 22> Stanza::Error::Private::errorDescriptions { {
    { ErrorCond::BadRequest, QT_TR_NOOP("Bad request"),
      QT_TR_NOOP("The sender has sent XML that is malformed or that cannot be processed.") },
    { ErrorCond::Conflict, QT_TR_NOOP("Conflict"),
      QT_TR_NOOP(
          "Access cannot be granted because an existing resource or session exists with the same name or address.") },
    { ErrorCond::FeatureNotImplemented, QT_TR_NOOP("Feature not implemented"),
      QT_TR_NOOP(
          "The feature requested is not implemented by the recipient or server and therefore cannot be processed.") },
    { ErrorCond::Forbidden, QT_TR_NOOP("Forbidden"),
      QT_TR_NOOP("The requesting entity does not possess the required permissions to perform the action.") },
    { ErrorCond::Gone, QT_TR_NOOP("Gone"),
      QT_TR_NOOP("The recipient or server can no longer be contacted at this address.") },
    { ErrorCond::InternalServerError, QT_TR_NOOP("Internal server error"),
      QT_TR_NOOP("The server could not process the stanza because of a misconfiguration or an otherwise-undefined "
                 "internal server error.") },
    { ErrorCond::ItemNotFound, QT_TR_NOOP("Item not found"),
      QT_TR_NOOP("The addressed JID or item requested cannot be found.") },
    { ErrorCond::JidMalformed, QT_TR_NOOP("JID malformed"),
      QT_TR_NOOP("The sending entity has provided or communicated an XMPP address (e.g., a value of the 'to' "
                 "attribute) or aspect thereof (e.g., a resource identifier) that does not adhere to the syntax "
                 "defined in Addressing Scheme.") },
    { ErrorCond::NotAcceptable, QT_TR_NOOP("Not acceptable"),
      QT_TR_NOOP("The recipient or server understands the request but is refusing to process it because it does not "
                 "meet criteria defined by the recipient or server (e.g., a local policy regarding acceptable words in "
                 "messages).") },
    { ErrorCond::NotAllowed, QT_TR_NOOP("Not allowed"),
      QT_TR_NOOP("The recipient or server does not allow any entity to perform the action.") },
    { ErrorCond::NotAuthorized, QT_TR_NOOP("Not authorized"),
      QT_TR_NOOP("The sender must provide proper credentials before being allowed to perform the action, or has "
                 "provided improper credentials.") },
    { ErrorCond::PolicyViolation, QT_TR_NOOP("Policy violation"),
      QT_TR_NOOP("The sender has violated some service policy.") },
    { ErrorCond::RecipientUnavailable, QT_TR_NOOP("Recipient unavailable"),
      QT_TR_NOOP("The intended recipient is temporarily unavailable.") },
    { ErrorCond::Redirect, QT_TR_NOOP("Redirect"),
      QT_TR_NOOP("The recipient or server is redirecting requests for this information to another entity, usually "
                 "temporarily.") },
    { ErrorCond::RegistrationRequired, QT_TR_NOOP("Registration required"),
      QT_TR_NOOP("The requesting entity is not authorized to access the requested service because registration is "
                 "required.") },
    { ErrorCond::RemoteServerNotFound, QT_TR_NOOP("Remote server not found"),
      QT_TR_NOOP(
          "A remote server or service specified as part or all of the JID of the intended recipient does not exist.") },
    { ErrorCond::RemoteServerTimeout, QT_TR_NOOP("Remote server timeout"),
      QT_TR_NOOP("A remote server or service specified as part or all of the JID of the intended recipient (or "
                 "required to fulfill a request) could not be contacted within a reasonable amount of time.") },
    { ErrorCond::ResourceConstraint, QT_TR_NOOP("Resource constraint"),
      QT_TR_NOOP("The server or recipient lacks the system resources necessary to service the request.") },
    { ErrorCond::ServiceUnavailable, QT_TR_NOOP("Service unavailable"),
      QT_TR_NOOP("The server or recipient does not currently provide the requested service.") },
    { ErrorCond::SubscriptionRequired, QT_TR_NOOP("Subscription required"),
      QT_TR_NOOP("The requesting entity is not authorized to access the requested service because a subscription is "
                 "required.") },
    { ErrorCond::UndefinedCondition, QT_TR_NOOP("Undefined condition"),
      QT_TR_NOOP("The error condition is not one of those defined by the other conditions in this list.") },
    { ErrorCond::UnexpectedRequest, QT_TR_NOOP("Unexpected request"),
      QT_TR_NOOP("The recipient or server understood the request but was not expecting it at this time (e.g., the "
                 "request was out of order).") },
} };

/**
    \brief Returns the error code

    If the error object was constructed with a code, this code will be returned.
    Otherwise, the code will be guessed.

    0 means unknown code.
*/
int Stanza::Error::code() const { return originalCode ? originalCode : Private::errorTypeCondToCode(type, condition); }

/**
    \brief Creates a StanzaError from \a code.

    The error's type and condition are guessed from the give \a code.
    The application-specific error element is preserved.
*/
bool Stanza::Error::fromCode(int code)
{
    auto guess = Private::errorCodeToTypeCond(code);
    if (!guess.has_value())
        return false;

    type         = guess->first;
    condition    = guess->second;
    originalCode = code;

    return true;
}

/**
    \brief Reads the error from XML

    This function finds and reads the error element \a e.

    You need to provide the base namespace of the stream which this stanza belongs to
    (probably by using stream.baseNS() function).
*/
bool Stanza::Error::fromXml(const QDomElement &e, const QString &baseNS)
{
    if (e.tagName() != "error" && e.namespaceURI() != baseNS)
        return false;

    // type
    auto parsedType = Private::stringToErrorType(e.attribute("type"));
    if (!parsedType.has_value()) {
        // code. deprecated. rfc6120 has just a little note about it. also see XEP-0086
        bool ok;
        originalCode = e.attribute("code").toInt(&ok);
        if (ok && originalCode) {
            auto guess = Private::errorCodeToTypeCond(originalCode);
            if (guess.has_value()) {
                type      = guess->first;
                condition = guess->second;
            } else {
                ok = false;
            }
        }
        if (!ok) {
            qWarning("unexpected error type=%s", qUtf8Printable(e.attribute("type")));
            return false;
        }
    } else {
        type      = *parsedType;
        condition = ErrorCond(-1);
    }

    by = e.attribute(QStringLiteral("by"));
    QString textTag(QStringLiteral("text"));
    for (auto t = e.firstChildElement(); !t.isNull(); t = t.nextSiblingElement()) {
        if (t.namespaceURI() == NS_STANZAS) {
            if (t.tagName() == textTag) {
                text = t.text().trimmed();
            } else {
                auto parsedCond = Private::stringToErrorCond(t.tagName());
                if (parsedCond.has_value()) {
                    condition = *parsedCond;
                }
            }
        } else {
            appSpec = t;
        }

        if (condition != ErrorCond(-1) && !appSpec.isNull() && !text.isEmpty())
            break;
    }

    // try to guess type/condition
    if (condition == ErrorCond(-1)) {
        condition = ErrorCond::UndefinedCondition;
    }

    return true;
}

/**
    \brief Writes the error to XML

    This function creates an error element representing the error object.

    You need to provide the base namespace of the stream to which this stanza belongs to
    (probably by using stream.baseNS() function).
*/
QDomElement Stanza::Error::toXml(QDomDocument &doc, const QString &baseNS) const
{
    QDomElement errElem = doc.createElementNS(baseNS, "error");
    QDomElement t;

    // XMPP error
    QString stype = Private::errorTypeToString(type);
    if (stype.isEmpty())
        return errElem;
    QString scond = Private::errorCondToString(condition);
    if (scond.isEmpty())
        return errElem;

    errElem.setAttribute("type", stype);
    if (!by.isEmpty()) {
        errElem.setAttribute("by", by);
    }
    errElem.appendChild(t = doc.createElementNS(NS_STANZAS, scond));
    // t.setAttribute("xmlns", NS_STANZAS);    // FIX-ME: this shouldn't be needed

    // old code
    int scode = code();
    if (scode)
        errElem.setAttribute("code", scode);

    // text
    if (!text.isEmpty()) {
        t = doc.createElementNS(NS_STANZAS, "text");
        // t.setAttribute("xmlns", NS_STANZAS);    // FIX-ME: this shouldn't be needed
        t.appendChild(doc.createTextNode(text));
        errElem.appendChild(t);
    }

    // application specific
    errElem.appendChild(appSpec);

    return errElem;
}

/**
    \brief Returns the error name and description

    Returns the error name (e.g. "Not Allowed") and generic description.
*/
QPair<QString, QString> Stanza::Error::description() const { return Private::errorCondToDesc(condition); }

/**
 * \brief Returns string human-reabable representation of the error
 */
QString Stanza::Error::toString() const
{
    QPair<QString, QString> desc = description();
    if (text.isEmpty())
        return desc.first + ".\n" + desc.second;
    else
        return desc.first + ".\n" + desc.second + "\n" + text;
}

//----------------------------------------------------------------------------
// Stanza
//----------------------------------------------------------------------------
class Stanza::Private {
public:
    static int stringToKind(const QString &s)
    {
        if (s == QStringLiteral("message"))
            return Message;
        else if (s == QStringLiteral("presence"))
            return Presence;
        else if (s == QStringLiteral("iq"))
            return IQ;
        else
            return -1;
    }

    static QString kindToString(Kind k)
    {
        if (k == Message)
            return QStringLiteral("message");
        else if (k == Presence)
            return QStringLiteral("presence");
        else
            return QStringLiteral("iq");
    }

    Stream                      *s;
    QDomElement                  e;
    QSharedPointer<QDomDocument> sharedDoc;
};

Stanza::Stanza() { d = nullptr; }

Stanza::Stanza(Stream *s, Kind k, const Jid &to, const QString &type, const QString &id)
{
    Q_ASSERT(s);
    d = new Private;

    Kind kind;
    if (k == Message || k == Presence || k == IQ)
        kind = k;
    else
        kind = Message;

    d->s = s;
    if (d->s)
        d->e = d->s->doc().createElementNS(s->baseNS(), Private::kindToString(kind));
    if (to.isValid())
        setTo(to);
    if (!type.isEmpty())
        setType(type);
    if (!id.isEmpty())
        setId(id);
}

Stanza::Stanza(Stream *s, const QDomElement &e)
{
    Q_ASSERT(s);
    d = nullptr;
    if (e.namespaceURI() != s->baseNS())
        return;
    int x = Private::stringToKind(e.tagName());
    if (x == -1)
        return;
    d    = new Private;
    d->s = s;
    d->e = e;
}

Stanza::Stanza(const Stanza &from)
{
    d     = nullptr;
    *this = from;
}

Stanza &Stanza::operator=(const Stanza &from)
{
    if (&from == this)
        return *this;

    delete d;
    d = nullptr;
    if (from.d)
        d = new Private(*from.d);
    return *this;
}

Stanza::~Stanza() { delete d; }

bool Stanza::isNull() const { return d == nullptr; }

QDomElement Stanza::element() const { return d->e; }

QString Stanza::toString() const { return Stream::xmlToString(d->e); }

QDomDocument &Stanza::doc() const { return d->s->doc(); }

QString Stanza::baseNS() const { return d->s->baseNS(); }

QDomElement Stanza::createElement(const QString &ns, const QString &tagName)
{
    return d->s->doc().createElementNS(ns, tagName);
}

QDomElement Stanza::createTextElement(const QString &ns, const QString &tagName, const QString &text)
{
    QDomElement e = d->s->doc().createElementNS(ns, tagName);
    e.appendChild(d->s->doc().createTextNode(text));
    return e;
}

void Stanza::appendChild(const QDomElement &e) { d->e.appendChild(e); }

Stanza::Kind Stanza::kind() const { return (Kind)Private::stringToKind(d->e.tagName()); }

Stanza::Kind Stanza::kind(const QString &tagName) { return (Kind)Private::stringToKind(tagName); }

void Stanza::setKind(Kind k) { d->e.setTagName(Private::kindToString(k)); }

Jid Stanza::to() const { return Jid(d->e.attribute("to")); }

Jid Stanza::from() const { return Jid(d->e.attribute("from")); }

QString Stanza::id() const { return d->e.attribute("id"); }

QString Stanza::type() const { return d->e.attribute("type"); }

QString Stanza::lang() const { return d->e.attributeNS(NS_XML, "lang", QString()); }

void Stanza::setTo(const Jid &j) { d->e.setAttribute("to", j.full()); }

void Stanza::setFrom(const Jid &j) { d->e.setAttribute("from", j.full()); }

void Stanza::setId(const QString &id) { d->e.setAttribute("id", id); }

void Stanza::setType(const QString &type) { d->e.setAttribute("type", type); }

void Stanza::setLang(const QString &lang) { d->e.setAttribute("xml:lang", lang); }

Stanza::Error Stanza::error() const
{
    Error       err;
    QDomElement e = d->e.elementsByTagNameNS(d->s->baseNS(), "error").item(0).toElement();
    if (!e.isNull())
        err.fromXml(e, d->s->baseNS());

    return err;
}

void Stanza::setError(const Error &err)
{
    QDomDocument doc     = d->e.ownerDocument();
    QDomElement  errElem = err.toXml(doc, d->s->baseNS());

    QDomElement oldElem = d->e.elementsByTagNameNS(d->s->baseNS(), "error").item(0).toElement();
    if (oldElem.isNull()) {
        d->e.appendChild(errElem);
    } else {
        d->e.replaceChild(errElem, oldElem);
    }
}

void Stanza::clearError()
{
    QDomElement errElem = d->e.elementsByTagNameNS(d->s->baseNS(), "error").item(0).toElement();
    if (!errElem.isNull())
        d->e.removeChild(errElem);
}

QSharedPointer<QDomDocument> Stanza::unboundDocument(QSharedPointer<QDomDocument> sd)
{
    if (!sd) {
        sd = QSharedPointer<QDomDocument>(new QDomDocument);
    }
    d->e         = sd->importNode(d->e, true).toElement();
    d->sharedDoc = sd;
    return d->sharedDoc;
}
