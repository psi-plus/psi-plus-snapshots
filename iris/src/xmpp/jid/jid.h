/*
 * jid.h
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

#ifndef XMPP_JID_H
#define XMPP_JID_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <memory>

namespace XMPP {
class StringPrepCache {
public:
    static bool nameprep(const QString &in, int maxbytes, QString &out);
    static bool nodeprep(const QString &in, int maxbytes, QString &out);
    static bool resourceprep(const QString &in, int maxbytes, QString &out);
    static bool saslprep(const QString &in, int maxbytes, QString &out);

    static void cleanup();

private:
    QHash<QString, QString> nameprep_table;
    QHash<QString, QString> nodeprep_table;
    QHash<QString, QString> resourceprep_table;
    QHash<QString, QString> saslprep_table;

    static std::unique_ptr<StringPrepCache> _instance;
    static StringPrepCache                 *instance();

    StringPrepCache();
};

class Jid {
public:
    Jid();
    ~Jid();

    Jid(const QString &s);
    Jid(const QString &node, const QString &domain, const QString &resource = "");
    Jid(const char *s);
    Jid &operator=(const QString &s);
    Jid &operator=(const char *s);

    bool           isNull() const { return null; }
    const QString &domain() const { return d; }
    const QString &node() const { return n; }
    const QString &resource() const { return r; }
    const QString &bare() const { return b; }
    const QString &full() const { return f; }

    Jid withNode(const QString &s) const;
    Jid withDomain(const QString &s) const;
    Jid withResource(const QString &s) const;

    bool        isValid() const;
    bool        isEmpty() const;
    bool        compare(const Jid &a, bool compareRes = true) const;
    inline bool operator==(const Jid &other) const { return compare(other, true); }
    inline bool operator!=(const Jid &other) const { return !(*this == other); }

private:
    void set(const QString &s);
    void set(const QString &domain, const QString &node, const QString &resource = "");

    void setDomain(const QString &s);
    void setNode(const QString &s);
    void setResource(const QString &s);

private:
    void reset();
    void update();

    QString f, b, d, n, r;
    bool    valid, null;
};

Q_DECL_PURE_FUNCTION inline uint qHash(const XMPP::Jid &key, uint seed = 0) Q_DECL_NOTHROW
{
    return qHash(key.full(), seed);
}
} // namespace XMPP

#endif // XMPP_JID_H
