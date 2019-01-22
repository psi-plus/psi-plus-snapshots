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

#ifndef XMPP_HASH_H
#define XMPP_HASH_H

#include "xmpp_stanza.h"

#include <QString>
#define XMPP_HASH_NS "urn:xmpp:hashes:2" // TODO make nsdb.cpp/h with static declarations of all ns

class QDomElement;

namespace XMPP
{
    class Features;

    class Hash
    {
    public:
        enum Type { // XEP-0300 Version 0.5.3 (2018-02-14)
            Unknown,    // not standard, just a default
            Sha1,       // SHOULD NOT
            Sha256,     // MUST
            Sha512,     // SHOULD
            Sha3_256,   // MUST
            Sha3_512,   // SHOULD
            Blake2b256, // MUST
            Blake2b512, // SHOULD
        };

        inline Hash(Type type = Type::Unknown) : v_type(type) {}
        Hash(const QDomElement&);

        inline Type type() const { return v_type; }
        inline void setType(Type t) { v_type = t; }

        inline QByteArray data() const { return v_data; }
        inline void setData(const QByteArray &d) { v_data = d; } // sets already computed hash
        bool computeFromData(const QByteArray &); // computes hash from passed data

        QDomElement toXml(Stanza&) const;
        static void populateFeatures(XMPP::Features &);

    private:
        Type v_type = Type::Unknown;
        QByteArray v_data;
    };
}

#endif
