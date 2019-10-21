/*
 * xmpp_form.h - XMPP form
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

#pragma once 

#include <QList>
#include <qdatetime.h>

namespace XMPP {
typedef QList<AgentItem> AgentList;
typedef QList<DiscoItem> DiscoList;

class FormField {
public:
    enum { username, nick, password, name, first, last, email, address, city, state, zip, phone, url, date, misc };
    FormField(const QString &type = "", const QString &value = "");
    ~FormField();

    int            type() const;
    QString        fieldName() const;
    QString        realName() const;
    bool           isSecret() const;
    const QString &value() const;
    void           setType(int);
    bool           setType(const QString &);
    void           setValue(const QString &);

private:
    int     tagNameToType(const QString &) const;
    QString typeToTagName(int) const;

    int     v_type;
    QString v_value;

    class Private;
    Private *d = nullptr;
};

class Form : public QList<FormField> {
public:
    Form(const Jid &j = "");
    ~Form();

    Jid     jid() const;
    QString instructions() const;
    QString key() const;
    void    setJid(const Jid &);
    void    setInstructions(const QString &);
    void    setKey(const QString &);

private:
    Jid     v_jid;
    QString v_instructions, v_key;

    class Private;
    Private *d = nullptr;
};

class SearchResult {
public:
    SearchResult(const Jid &jid = "");
    ~SearchResult();

    const Jid &    jid() const;
    const QString &nick() const;
    const QString &first() const;
    const QString &last() const;
    const QString &email() const;

    void setJid(const Jid &);
    void setNick(const QString &);
    void setFirst(const QString &);
    void setLast(const QString &);
    void setEmail(const QString &);

private:
    Jid     v_jid;
    QString v_nick, v_first, v_last, v_email;
};
} // namespace XMPP
