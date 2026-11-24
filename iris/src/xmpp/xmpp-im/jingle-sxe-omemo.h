/*
 * jingle-sxe-omemo.h - SXE payload protected with OMEMO 2
 * Copyright (C) 2026  Sergey Ilinykh
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

#ifndef JINGLE_SXE_OMEMO_H
#define JINGLE_SXE_OMEMO_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-session.h>

namespace XMPP::Jingle::SXE {
    // Wire-format adapter only. Encryption state and lifetime belong to the Jingle session.
    class IRIS_EXPORT SxeOmemo {
    public:
        using Done = std::function<void(const QDomElement &)>;
        static QString feature();
        static QString methodId();
        static QString wireProfile();
        static bool isSupported(XMPP::Jingle::Manager *manager);
        static bool canProtect(Session *session);
        static void protect(Session *session, const QDomElement &sxe, QObject *receiver, const Done &done);
        static void unprotect(Session *session, const Jid &from, const QDomElement &message, QObject *receiver,
                              const Done &done);
    };
}

#endif // JINGLE_SXE_OMEMO_H
