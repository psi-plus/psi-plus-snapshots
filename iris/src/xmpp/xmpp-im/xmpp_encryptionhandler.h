/*
 * xmpp_encryptionhandler.h
 * Copyright (C) 2018  Vyacheslav Karpukhin
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

#ifndef PSI_XMPP_ENCRYPTIONHANDLER_H
#define PSI_XMPP_ENCRYPTIONHANDLER_H

class QDomElement;

namespace XMPP {
    class EncryptionHandler
    {
    public:
        virtual bool decryptMessageElement(QDomElement &) = 0;
        virtual bool encryptMessageElement(QDomElement &) = 0;
    };
} // namespace XMPP

#endif //PSI_XMPP_ENCRYPTIONHANDLER_H
