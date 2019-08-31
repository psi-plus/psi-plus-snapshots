/*
 * Copyright (C) 2010  Tobias Markmann
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

#ifndef SCRAMSHA1SIGNATURE_H
#define SCRAMSHA1SIGNATURE_H

#include <QByteArray>
#include <QString>
#include <QtCrypto>

namespace XMPP {
    class SCRAMSHA1Signature
    {
        public:
            SCRAMSHA1Signature(const QByteArray &server_final_message, const QCA::SecureArray &server_signature_should);

            bool isValid() const {
                return isValid_;
            }

        private:
            bool isValid_;
    };
} // namespace XMPP

#endif // SCRAMSHA1SIGNATURE_H
