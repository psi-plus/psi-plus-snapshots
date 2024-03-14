/*
 * tasks.h - basic tasks
 * Copyright (C) 2001-2002  Justin Karneges
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

#ifndef XMPP_TASKS_H
#define XMPP_TASKS_H

#include "xmpp_discoinfotask.h"
#include "xmpp_encryptionhandler.h"
#include "xmpp_form.h"
#include "xmpp_message.h"
#include "xmpp_subsets.h"
#include "xmpp_vcard.h"

#include <QList>
#include <QString>
#include <QtXml>

namespace XMPP {
class BoBData;
class CaptchaChallenge;
class Roster;
class Status;

class JT_Register : public Task {
    Q_OBJECT
public:
    JT_Register(Task *parent);
    ~JT_Register();

    // OLd style registration
    void reg(const QString &user, const QString &pass);

    void changepw(const QString &pass);
    void unreg(const Jid &j = "");

    const Form  &form() const;
    bool         hasXData() const;
    const XData &xdata() const;
    bool         isRegistered() const;
    void         getForm(const Jid &);
    void         setForm(const Form &);
    void         setForm(const Jid &, const XData &);

    void onGo();
    bool take(const QDomElement &);

private:
    QDomElement iq;
    Jid         to;

    class Private;
    Private *d = nullptr;
};

class JT_UnRegister : public Task {
    Q_OBJECT
public:
    JT_UnRegister(Task *parent);
    ~JT_UnRegister();

    void unreg(const Jid &);

    void onGo();

private slots:
    void getFormFinished();
    void unregFinished();

private:
    class Private;
    Private *d = nullptr;
};

class JT_Roster : public Task {
    Q_OBJECT
public:
    JT_Roster(Task *parent);
    ~JT_Roster();

    void get();
    void set(const Jid &, const QString &name, const QStringList &groups);
    void remove(const Jid &);

    // XEP-0083
    void getGroupsDelimiter();
    void setGroupsDelimiter(const QString &groupsDelimiter);

    const Roster &roster() const;
    QString       groupsDelimiter() const;

    QString toString() const;
    bool    fromString(const QString &);

    void onGo();
    bool take(const QDomElement &x);

private:
    enum Type { Get, Set, Remove, GetDelimiter, SetDelimiter, Unknown = -1 };

    int         type;
    QDomElement iq;
    Jid         to;

    class Private;
    Private *d = nullptr;
};

class JT_PushRoster : public Task {
    Q_OBJECT
public:
    JT_PushRoster(Task *parent);
    ~JT_PushRoster();

    bool take(const QDomElement &);

signals:
    void roster(const Roster &);

private:
    class Private;
    Private *d = nullptr;
};

class JT_Presence : public Task {
    Q_OBJECT
public:
    JT_Presence(Task *parent);
    ~JT_Presence();

    void pres(const Status &);
    void pres(const Jid &, const Status &);
    void sub(const Jid &, const QString &subType, const QString &nick = QString());
    void probe(const Jid &to);

    void onGo();

private:
    QDomElement tag;
    int         type = -1;

    class Private;
    Private *d = nullptr;
};

class JT_PushPresence : public Task {
    Q_OBJECT
public:
    JT_PushPresence(Task *parent);
    ~JT_PushPresence();

    bool take(const QDomElement &);

signals:
    void presence(const Jid &, const Status &);
    void subscription(const Jid &, const QString &, const QString &);

private:
    class Private;
    Private *d = nullptr;
};

class JT_Session : public Task {
public:
    JT_Session(Task *parent);
    void onGo();
    bool take(const QDomElement &);
};

class JT_Message : public Task {
    Q_OBJECT
public:
    JT_Message(Task *parent, Message &);
    ~JT_Message();

    void onGo();

private:
    Message m;

    class Private;
    Private *d = nullptr;
};

class JT_PushMessage : public Task {
    Q_OBJECT
public:
    JT_PushMessage(Task *parent, EncryptionHandler *encryptionHandler = nullptr);
    ~JT_PushMessage();

    bool take(const QDomElement &);

signals:
    void message(const Message &);

private:
    class Private;
    Private *d = nullptr;
};

class JT_VCard : public Task {
    Q_OBJECT
public:
    JT_VCard(Task *parent);
    ~JT_VCard();

    void get(const Jid &);
    void set(const VCard &);
    void set(const Jid &, const VCard &, bool isTarget = false);

    const Jid   &jid() const;
    const VCard &vcard() const;

    void onGo();
    bool take(const QDomElement &x);

private:
    int type;

    class Private;
    Private *d = nullptr;
};

class JT_Search : public Task {
    Q_OBJECT
public:
    JT_Search(Task *parent);
    ~JT_Search();

    const Form                &form() const;
    const QList<SearchResult> &results() const;

    bool         hasXData() const;
    const XData &xdata() const;

    void get(const Jid &);
    void set(const Form &);
    void set(const Jid &, const XData &);

    void onGo();
    bool take(const QDomElement &x);

private:
    QDomElement iq;
    int         type;

    class Private;
    Private *d = nullptr;
};

class JT_ClientVersion : public Task {
    Q_OBJECT
public:
    JT_ClientVersion(Task *);

    void get(const Jid &);
    void onGo();
    bool take(const QDomElement &);

    const Jid     &jid() const;
    const QString &name() const;
    const QString &version() const;
    const QString &os() const;

private:
    QDomElement iq;

    Jid     j;
    QString v_name, v_ver, v_os;
};

class JT_EntityTime : public Task {
public:
    JT_EntityTime(Task *);

    void             onGo();
    bool             take(const QDomElement &);
    void             get(const XMPP::Jid &j);
    const XMPP::Jid &jid() const;

    const QDateTime &dateTime() const;
    int              timezoneOffset() const;

private:
    QDomElement iq;
    XMPP::Jid   j;
    QDateTime   utc;
    int         tzo = 0;
};

class JT_ServInfo : public Task {
    Q_OBJECT
public:
    JT_ServInfo(Task *);
    ~JT_ServInfo();

    bool take(const QDomElement &);
};

class JT_Gateway : public Task {
    Q_OBJECT
public:
    JT_Gateway(Task *);

    void get(const Jid &);
    void set(const Jid &, const QString &prompt);
    void onGo();
    bool take(const QDomElement &);

    Jid jid() const;

    QString desc() const;
    QString prompt() const;
    Jid     translatedJid() const;

private:
    QDomElement iq;

    int     type;
    Jid     v_jid;
    Jid     v_translatedJid;
    QString v_prompt, v_desc;
};

class JT_DiscoItems : public Task {
    Q_OBJECT
public:
    JT_DiscoItems(Task *);
    ~JT_DiscoItems();

    void get(const Jid &, const QString &node = QString());
    void get(const DiscoItem &);

    const DiscoList &items() const;

    void includeSubsetQuery(const SubsetsClientManager &);
    bool extractSubsetInfo(SubsetsClientManager &);

    void onGo();
    bool take(const QDomElement &);

private:
    class Private;
    Private *d = nullptr;
};

class JT_DiscoPublish : public Task {
    Q_OBJECT
public:
    JT_DiscoPublish(Task *);
    ~JT_DiscoPublish();

    void set(const Jid &, const DiscoList &);

    void onGo();
    bool take(const QDomElement &);

private:
    class Private;
    Private *d = nullptr;
};

class JT_BoBServer : public Task {
    Q_OBJECT

public:
    JT_BoBServer(Task *parent);
    bool take(const QDomElement &);
};

class JT_BitsOfBinary : public Task {
    Q_OBJECT
public:
    JT_BitsOfBinary(Task *);
    ~JT_BitsOfBinary();

    void get(const Jid &, const QString &);

    void     onGo();
    bool     take(const QDomElement &);
    BoBData &data();

private:
    class Private;
    Private *d = nullptr;
};

class JT_PongServer : public Task {
    Q_OBJECT
public:
    JT_PongServer(Task *);
    bool take(const QDomElement &);
};

class JT_MessageCarbons : public Task {
    Q_OBJECT

public:
    JT_MessageCarbons(Task *parent);
    void enable();
    void disable();

    void onGo();
    bool take(const QDomElement &e);

private:
    QDomElement _iq;
};

class JT_CaptchaChallenger : public Task {
    Q_OBJECT
public:
    const static int CaptchaValidTimeout = 120;

    JT_CaptchaChallenger(Task *);
    ~JT_CaptchaChallenger();

    void set(const Jid &, const CaptchaChallenge &);

    void onGo();
    bool take(const QDomElement &);

private:
    class Private;
    Private *d = nullptr;
};

class JT_CaptchaSender : public Task {
    Q_OBJECT
public:
    JT_CaptchaSender(Task *);

    void set(const Jid &, const XData &);

    void onGo();
    bool take(const QDomElement &);

private:
    Jid         to;
    QDomElement iq;
};
} // namespace XMPP

#endif // XMPP_TASKS_H
