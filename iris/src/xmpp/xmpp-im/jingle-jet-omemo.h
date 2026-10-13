/*
 * jingle-jet-omemo.h - Jingle Encrypted Transports with OMEMO (XEP-0391, XEP-0396)
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

#ifndef JINGLE_JET_OMEMO_H
#define JINGLE_JET_OMEMO_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle.h>

#include <QObject>

#include <memory>

namespace XMPP {
class OmemoEncryption;

namespace Jingle {
    /**
     * @brief End-to-end encrypted file transfer with JET-OMEMO, compatible with Conversations and Dino.
     *
     * For file-transfer contents of sessions that prefer or require end-to-end encryption
     * (Session::isEndToEndPreferred()), the creator of the content adds
     * <security xmlns='urn:xmpp:jingle:jet:0'> with an OMEMO envelope of the key for all devices of the
     * peer, and both sides give the key to FileTransfer::Application::setTransportSecret(). With the
     * legacy profile (Conversations, Dino) the envelope is a key transport message and the cipher
     * AES-128-GCM; with OMEMO 2, chosen when the peer advertises it, an SCE envelope holds an AES-256 key
     * (a Psi extension, docs/xsf-proposals/xep-0396-jet-omemo2.md).
     * The file then travels as AES-GCM over any transport (S5B, IBB, ICE).
     *
     * A session that only prefers encryption sends files unprotected to a peer without JET-OMEMO;
     * one that requires it refuses them. Register it before OmemoDtlsVerification, which treats
     * JET-protected contents as protected. docs/jingle-e2e.md describes the format.
     */
    class IRIS_EXPORT JetOmemo : public QObject, public SignalingFilter {
        Q_OBJECT
    public:
        static QString ns();    // urn:xmpp:jingle:jet-omemo:0
        static QString jetNs(); // urn:xmpp:jingle:jet:0

        /// Registers itself as a signaling filter of @a manager until destroyed.
        JetOmemo(Manager *manager, OmemoEncryption *omemo, QObject *parent = nullptr);
        ~JetOmemo() override;

        bool        filterIncoming(const Jid &from, const QDomElement &jingle, const Done &done) override;
        bool        filterOutgoing(const Jid &to, const QDomElement &jingle, const Done &done) override;
        QStringList discoFeatures() const override;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };
}
}

#endif // JINGLE_JET_OMEMO_H
