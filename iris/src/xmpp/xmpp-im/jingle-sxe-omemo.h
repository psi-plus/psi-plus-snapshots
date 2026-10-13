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
#include <iris/xmpp-im/jingle-sxe.h>

#include <QPointer>

namespace XMPP {
class OmemoEncryption;

namespace Jingle { namespace SXE {
    /**
     * @brief SXE payload in messages protected with OMEMO 2: each <sxe/> travels in an SCE envelope.
     *
     * Used for sessions that prefer end-to-end encryption when the peer advertises OMEMO 2 and
     * urn:xmpp:sxe:0#omemo (a Psi extension, docs/xsf-proposals/xep-0284-sxe-encryption.md).
     * Registers itself with the SXE transport manager of @a manager until destroyed.
     */
    class IRIS_EXPORT SxeOmemo : public QObject, public PayloadProtector {
        Q_OBJECT
    public:
        static QString feature(); // urn:xmpp:sxe:0#omemo

        SxeOmemo(XMPP::Jingle::Manager *manager, OmemoEncryption *omemo, QObject *parent = nullptr);
        ~SxeOmemo() override;

        QString     method() const override;
        bool        canProtect(const Jid &peer) const override;
        QStringList discoFeatures() const override;
        void        protect(const Jid &peer, const QDomElement &sxe, const Done &done) override;
        void        unprotect(const Jid &from, const QDomElement &message, const Done &done) override;

    private:
        QPointer<XMPP::Jingle::Manager> manager_;
        QPointer<OmemoEncryption>       omemo_;
    };
}}
}

#endif // JINGLE_SXE_OMEMO_H
