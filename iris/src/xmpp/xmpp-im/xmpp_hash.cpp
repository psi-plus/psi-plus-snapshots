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

#include "xmpp_hash.h"

#include "xmpp/blake2/blake2qt.h"
#include "xmpp_features.h"
#include "xmpp_xmlcommon.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <qca.h>

#include <array>
#include <variant>

namespace XMPP {

//----------------------------------------------------------------------------
// Hash
//----------------------------------------------------------------------------
static const char *const sha1_synonims[] = { "sha1", nullptr };
// NOTE: keep this in sync with enum. same order!
struct HashDesc {
    const char        *text;
    Hash::Type         hashType;
    const char *const *synonims = nullptr;
};

// hash types in priority order mostly by speed
static const std::array hashTypes {
    HashDesc { "blake2b-512", Hash::Type::Blake2b512 },
    HashDesc { "blake2b-256", Hash::Type::Blake2b256 },
    HashDesc { "sha-1", Hash::Type::Sha1, sha1_synonims },
    HashDesc { "sha-512", Hash::Type::Sha512 },
    HashDesc { "sha-256", Hash::Type::Sha256 },
    HashDesc { "sha3-512", Hash::Type::Sha3_512 },
    HashDesc { "sha3-256", Hash::Type::Sha3_256 },
}; // HashDesc { "unknown", Hash::Type::Unknown },

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
using HashVariant = std::variant<std::nullptr_t, QCryptographicHash, QCA::Hash, Blake2Hash>;
#else
using HashVariant = std::variant<std::nullptr_t, QCryptographicHash, QCA::Hash>;
#endif
HashVariant findHasher(Hash::Type hashType)
{
    QString                       qcaType;
    QCryptographicHash::Algorithm qtType = QCryptographicHash::Algorithm(-1);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    Blake2Hash::DigestSize blakeDS = Blake2Hash::DigestSize(-1);
#endif

    switch (hashType) {
    case Hash::Type::Sha1:
        qtType  = QCryptographicHash::Sha1;
        qcaType = "sha1";
        break;
    case Hash::Type::Sha256:
        qtType  = QCryptographicHash::Sha256;
        qcaType = "sha256";
        break;
    case Hash::Type::Sha512:
        qtType  = QCryptographicHash::Sha512;
        qcaType = "sha512";
        break;
    case Hash::Type::Sha3_256:
        qtType  = QCryptographicHash::Sha3_256;
        qcaType = "sha3_256";
        break;
    case Hash::Type::Sha3_512:
        qtType  = QCryptographicHash::Sha3_512;
        qcaType = "sha3_512";
        break;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    case Hash::Type::Blake2b256:
        qcaType = "blake2b_256";
        blakeDS = Blake2Hash::Digest256;
        break;
    case Hash::Type::Blake2b512:
        qcaType = "blake2b_512";
        blakeDS = Blake2Hash::Digest512;
        break;
#else
    case Hash::Type::Blake2b256:
        qtType  = QCryptographicHash::Blake2b_256;
        qcaType = "blake2b_256";
        break;
    case Hash::Type::Blake2b512:
        qtType  = QCryptographicHash::Blake2b_512;
        qcaType = "blake2b_512";
        break;
#endif
    case Hash::Type::Unknown:
    default:
        qDebug("invalid hash type");
        return nullptr;
    }

    if (!qcaType.isEmpty()) {
        QCA::Hash hashObj(qcaType);
        if (hashObj.context()) {
            return hashObj;
        }
    }

    if (qtType != QCryptographicHash::Algorithm(-1)) {
        return HashVariant { std::in_place_type<QCryptographicHash>, qtType };
    }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    else if (blakeDS != Blake2Hash::DigestSize(-1)) {
        Blake2Hash bh(blakeDS);
        if (bh.isValid()) {
            return HashVariant { std::in_place_type<Blake2Hash>, std::move(bh) };
        }
    }
#endif
    return nullptr;
}

Hash::Hash(const QDomElement &el)
{
    QString algo = el.attribute(QLatin1String("algo"));
    v_type       = parseType(QStringView { algo });
    if (v_type != Unknown && el.tagName() == QLatin1String("hash")) {
        v_data = QByteArray::fromBase64(el.text().toLatin1());
        if (v_data.isEmpty()) {
            v_type = Type::Unknown;
        }
    }
}

QString Hash::stringType() const
{
    if (!v_type || int(v_type) > LastType)
        return {}; // must be empty. other code relies on it
    static_assert(LastType == hashTypes.size(), "hashType and enum are not in sync");
    auto it = std::ranges::find_if(hashTypes, [this](auto const &v) { return v.hashType == v_type; });
    if (it == hashTypes.end()) {
        throw std::logic_error("hashTypes array is inconsistent");
    }
    return QLatin1String(it->text);
}

Hash::Type Hash::parseType(const QStringView &algo)
{
    if (!algo.isEmpty()) {
        for (auto const &hash : hashTypes) {
            if (algo == QLatin1String(hash.text)) {
                return hash.hashType;
            }
            if (hash.synonims) {
                auto cur = hash.synonims;
                while (*cur) {
                    if (algo == QLatin1String(*cur)) {
                        return hash.hashType;
                    }
                    cur++;
                }
            }
        }
    }
    return Unknown;
}

bool Hash::compute(const QByteArray &ba)
{
    v_data.clear();
    auto hasher = findHasher(v_type);
    std::visit(
        [&ba, this](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if constexpr (std::is_same_v<T, QCA::Hash>) {
                arg.update(ba);
                v_data = arg.final().toByteArray();
            } else if constexpr (std::is_same_v<T, QCryptographicHash>) {
                arg.addData(ba);
                v_data = arg.result();
            } else if constexpr (std::is_same_v<T, Blake2Hash>) {
                if (arg.addData(ba))
                    v_data = arg.final();
            }
#else
            // Qt6 claims to have openssl backend and it doesn't copy data (?)
            if constexpr (std::is_same_v<T, QCryptographicHash>) {
                arg.addData(ba);
                v_data = arg.result();
            } else if constexpr (std::is_same_v<T, QCA::Hash>) {
                arg.update(ba);
                v_data = arg.final().toByteArray();
            }
#endif
        },
        hasher);

    if (!v_data.isEmpty())
        return true;

    qDebug("failed to compute %s hash for %lld bytes", qPrintable(stringType()), qsizetype(ba.size()));
    return false;
}

bool Hash::compute(QIODevice *dev)
{
    v_data.clear();
    auto hasher = findHasher(v_type);
    std::visit(
        [dev, this](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if constexpr (std::is_same_v<T, QCA::Hash>) {
                arg.update(dev);
                v_data = arg.final().toByteArray();
            } else if constexpr (std::is_same_v<T, QCryptographicHash>) {
                arg.addData(dev);
                v_data = arg.result();
            } else if constexpr (std::is_same_v<T, Blake2Hash>) {
                if (arg.addData(dev))
                    v_data = arg.final();
            }
#else
            if constexpr (std::is_same_v<T, QCryptographicHash>) {
                arg.addData(dev);
                v_data = arg.result();
            } else if constexpr (std::is_same_v<T, QCA::Hash>) {
                arg.update(dev);
                v_data = arg.final().toByteArray();
            }
#endif
        },
        hasher);

    if (!v_data.isEmpty())
        return true;

    qDebug("failed to compute %s hash on device 0x%p", qPrintable(stringType()), dev);
    return false;
}

QDomElement Hash::toXml(QDomDocument *doc) const
{
    auto stype = stringType();
    if (!stype.isEmpty()) {
        auto el = doc->createElementNS(HASH_NS, QLatin1String(v_data.isEmpty() ? "hash-used" : "hash"));
        el.setAttribute(QLatin1String("algo"), stype);
        if (!v_data.isEmpty()) {
            XMLHelper::setTagText(el, v_data.toBase64());
        }
        return el;
    }
    return QDomElement();
}

void Hash::populateFeatures(Features &features)
{
    features.addFeature("urn:xmpp:hashes:2");
    for (auto const &hash : hashTypes) {
        features.addFeature(QLatin1String("urn:xmpp:hash-function-text-names:") + QLatin1String(hash.text));
    }
}

Hash Hash::from(XMPP::Hash::Type t, const QByteArray &fileData)
{
    Hash h(t);
    if (!h.compute(fileData))
        h.setType(Unknown);
    return h;
}

Hash Hash::from(XMPP::Hash::Type t, QIODevice *dev)
{
    Hash h(t);
    if (!h.compute(dev))
        h.setType(Unknown);
    return h;
}

Hash Hash::from(Hash::Type t, const QFileInfo &file)
{
    if (file.isReadable()) {
        QFile f(file.filePath());
        f.open(QIODevice::ReadOnly);
        return from(t, &f);
    }
    return Hash();
}

Hash Hash::from(const QStringView &str)
{
    auto ind = str.indexOf('+');
    if (ind <= 0)
        return Hash();
    Hash hash(str.left(ind));
    if (hash.isValid()) {
        auto data = QByteArray::fromHex(str.mid(ind + 1).toLatin1());
        if (data.size())
            hash.setData(data);
        else
            hash = Hash();
    }
    return hash;
}

Hash Hash::fastestHash(const Features &features)
{
    for (auto const &h : hashTypes) {
        auto feature = QString(QLatin1String("urn:xmpp:hash-function-text-names:")) + QLatin1String(h.text);
        if (features.test(feature)) {
            return Hash(h.hashType);
        }
    }
    return {};
}

class StreamHashPrivate {
public:
    Hash::Type  type;
    HashVariant hasher;
    StreamHashPrivate(Hash::Type type) : type(type), hasher(findHasher(type)) { }
};

StreamHash::StreamHash(Hash::Type type) : d(new StreamHashPrivate(type)) { }

StreamHash::~StreamHash() { }

bool StreamHash::addData(const QByteArray &data)
{
    if (data.isEmpty())
        return true;

    bool ret = true;
    std::visit(
        [&data, &ret](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if constexpr (std::is_same_v<T, QCA::Hash>) {
                arg.update(data);
            } else if constexpr (std::is_same_v<T, QCryptographicHash>) {
                arg.addData(data);
            } else if constexpr (std::is_same_v<T, Blake2Hash>) {
                ret = arg.addData(data);
            }
#else
            if constexpr (std::is_same_v<T, QCryptographicHash>) {
                arg.addData(data);
            } else if constexpr (std::is_same_v<T, QCA::Hash>) {
                arg.update(data);
            }
#endif
            else
                ret = false;
        },
        d->hasher);
    return ret;
}

Hash StreamHash::final()
{
    auto data = std::visit(
        [](auto &&arg) {
            using T = std::decay_t<decltype(arg)>;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            if constexpr (std::is_same_v<T, QCA::Hash>) {
                return arg.final().toByteArray();
            } else if constexpr (std::is_same_v<T, QCryptographicHash>) {
                return arg.result();
            } else if constexpr (std::is_same_v<T, Blake2Hash>) {
                return arg.final();
            }
#else
            if constexpr (std::is_same_v<T, QCryptographicHash>) {
                return arg.result();
            } else if constexpr (std::is_same_v<T, QCA::Hash>) {
                return arg.final().toByteArray();
            }
#endif
            return QByteArray();
        },
        d->hasher);

    Hash h(d->type, data);
    if (data.isEmpty()) {
        qDebug("failed to compute %s hash on a stream", qPrintable(h.stringType()));
        return {};
    }
    return h;
}

void StreamHash::restart() { d.reset(new StreamHashPrivate(d->type)); }

} // namespace XMPP
