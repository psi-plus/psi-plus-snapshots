/*
 * jignle.h - General purpose Jingle
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

#include "xmpp_hash.h"

#include <QSharedDataPointer>
#include <QSharedPointer>

class QDomElement;
class QDomDocument;

namespace XMPP {
class Client;

namespace Jingle {

extern const QString NS;

class Manager;
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

    Jingle();
    Jingle(Manager *manager, const QDomElement &e);
    Jingle(const Jingle &);
    ~Jingle();
    QDomElement toXml(QDomDocument *doc) const;
    inline bool isValid() const { return d != nullptr; }
    Action action() const;
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

    Reason();
    ~Reason();
    Reason(const QDomElement &el);
    Reason(const Reason &other);
    inline bool isValid() const { return d != nullptr; }
    Condition condition() const;
    void setCondition(Condition cond);
    QString text() const;
    void setText(const QString &text);

    QDomElement toXml(QDomDocument *doc) const;

private:
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

class ContentBase {
public:
    enum class Creator {
        NoCreator, // not standard, just a default
        Initiator,
        Responder
    };

    enum class Senders {
        Both, // it's default
        None,
        Initiator,
        Responder
    };

    inline ContentBase(){}
    ContentBase(const QDomElement &el);

    inline bool isValid() const { return creator != Creator::NoCreator && !name.isEmpty(); }
protected:
    QDomElement toXml(QDomDocument *doc, const char *tagName) const;
    static Creator creatorAttr(const QDomElement &el);
    static bool setCreatorAttr(QDomElement &el, Creator creator);

    Creator creator = Creator::NoCreator;
    QString name;
    Senders senders = Senders::Both;
    QString disposition; // default "session"
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

class TransportManager
{
public:
    /*
    Categorization by speed, reliability and connectivity
    - speed: realtim, fast, slow
    - reliability: reliable, not reliable
    - connectivity: always connect, hard to connect

    Some transports may change their qualities, so we have to consider worst case.

    ICE-UDP: RealTime, Not Reliable, Hard To Connect
    S5B:     Fast,     Reliable,     Hard To Connect
    IBB:     Slow,     Reliable,     Always Connect
    */
    enum Feature {
        // connection establishment
        HardToConnect = 0x01,
        AlwaysConnect = 0x02,

        // reliability
        NotReliable   = 0x10,
        Reliable      = 0x20,

        // speed
        Slow          = 0x100,
        Fast          = 0x200,
        RealTime      = 0x400
    };

    Q_DECLARE_FLAGS(Features, Feature)
};

class Security
{

};

class Content : public ContentBase // TODO that's somewhat wrong mixing pimpl with this base
{
public:

    inline Content(){}
    Content(Manager *manager, const QDomElement &content);
    QDomElement toXml(QDomDocument *doc) const;

    QSharedPointer<Description> description;
    QSharedPointer<TransportManager> transport;
    QSharedPointer<Security> security;
    Reason reason;
};

class Manager;
class Session : public QObject
{
    Q_OBJECT
public:
    Session(Manager *manager);
    ~Session();

    void initiate(const Content &content);
private:
    class Private;
    QScopedPointer<Private> d;
};

class Application : public QObject
{
    Q_OBJECT
public:
    Application(Client *client);
    virtual ~Application();

    Client *client() const;
    virtual void incomingSession(Session *session) = 0;

    virtual QSharedPointer<Description> descriptionFromXml(const QDomElement &el) = 0;

private:
    class Private;
    QScopedPointer<Private> d;
};

class Manager : public QObject
{
    Q_OBJECT

	static const int MaxSessions = 1000; //1000? just to have some limit

public:
	explicit Manager(XMPP::Client *client = 0);
	~Manager();

	XMPP::Client* client() const;
	//Session* sessionInitiate(const Jid &to, const QDomElement &description, const QDomElement &transport);
	// TODO void setRedirection(const Jid &to);

	void registerApp(const QString &ns, Application *app);

	Session* newSession(const Jid &j);

    QSharedPointer<Description> descriptionFromXml(const QDomElement &el);
    QSharedPointer<TransportManager> transportFromXml(const QDomElement &el);

private:
    friend class JTPush;
    bool incomingIQ(const QDomElement &iq);

    class Private;
	QScopedPointer<Private> d;
};

} // namespace Jingle
} // namespace XMPP

#endif // JINGLE_H
