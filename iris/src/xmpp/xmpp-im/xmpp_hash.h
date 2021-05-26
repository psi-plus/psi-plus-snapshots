/*
 * Copyright (C) 2019-2021  Sergey Ilinykh
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

#ifndef XMPP_HASH_H
#define XMPP_HASH_H

#include <QHash>
#include <QString>

#include <memory>

class QDomElement;
class QFileInfo;
class QIODevice;
class QDomDocument;

namespace XMPP {

extern QString HASH_NS;
class Features;

class Hash {
public:
    enum Type {     // XEP-0300 Version 0.5.3 (2018-02-14)
        Unknown,    // not standard, just a default
        Sha1,       // SHOULD NOT
        Sha256,     // MUST
        Sha512,     // SHOULD
        Sha3_256,   // MUST
        Sha3_512,   // SHOULD
        Blake2b256, // MUST
        Blake2b512, // SHOULD
        LastType = Blake2b512
    };

    inline Hash(Type type = Type::Unknown) : v_type(type) { }
    inline Hash(Type type, const QByteArray &data) : v_type(type), v_data(data) { }
    inline Hash(const QStringRef &algo) : v_type(parseType(algo)) { }
    Hash(const QDomElement &);

    inline bool operator==(const Hash &other) const { return v_type == other.v_type && v_data == other.v_data; }

    inline bool isValid() const { return v_type > Unknown && v_type <= LastType; }
    inline      operator bool() const { return isValid(); }

    inline Type type() const { return v_type; }
    inline void setType(Type t) { v_type = t; }
    QString     stringType() const;
    static Type parseType(const QStringRef &algo);

    inline QByteArray data() const { return v_data; }
    inline void       setData(const QByteArray &d) { v_data = d; } // sets already computed hash
    inline QByteArray toHex() const { return v_data.toHex(); }
    inline QByteArray toBase64() const { return v_data.toBase64(); }
    inline QString toString() const { return QString("%1+%2").arg(stringType(), QString::fromLatin1(v_data.toHex())); }
    bool           compute(const QByteArray &); // computes hash from passed data
    bool           compute(QIODevice *dev);     // reads all the device and computes hash from the data

    QDomElement toXml(QDomDocument *doc) const;
    static void populateFeatures(XMPP::Features &);
    static Hash from(Type t, const QByteArray &fileData);
    static Hash from(XMPP::Hash::Type t, QIODevice *dev);
    static Hash from(XMPP::Hash::Type t, const QFileInfo &file);
    static Hash from(const QStringRef &str); // e.g. sha1+aabccddeeffaabbcc232387539465923645
    static Hash fastestHash(const Features &features);

private:
    Type       v_type = Type::Unknown;
    QByteArray v_data;
};

class StreamHashPrivate;
class StreamHash {
public:
    StreamHash(Hash::Type type);
    ~StreamHash();
    bool addData(const QByteArray &data);
    Hash final();
    void restart();

private:
    friend class StreamHashPrivate;
    std::unique_ptr<StreamHashPrivate> d;
};

Q_DECL_PURE_FUNCTION inline uint qHash(const Hash &hash, uint seed = 0) Q_DECL_NOTHROW
{
    return qHash(hash.data(), seed);
}

} // namespace XMPP

#endif // XMPP_HASH_H
