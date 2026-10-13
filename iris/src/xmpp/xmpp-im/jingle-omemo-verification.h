/*
 * jingle-omemo-verification.h - OMEMO-verified DTLS fingerprints in Jingle
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

#ifndef JINGLE_OMEMO_VERIFICATION_H
#define JINGLE_OMEMO_VERIFICATION_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle.h>

#include <QObject>

#include <memory>
#include <optional>

namespace XMPP {
class OmemoEncryption;

namespace Jingle {
    /**
     * @brief OMEMO-verified DTLS fingerprints, compatible with Conversations and Dino.
     *
     * The DTLS fingerprints of ICE transports (ice:0 and ice-udp:1) travel encrypted with OMEMO for
     * one device of the peer, so a server cannot replace them: the media or data channel is then
     * end-to-end protected without a second encryption layer. Wire format (docs/jingle-e2e.md):
     * <fingerprint xmlns='http://gultsch.de/xmpp/drafts/omemo/dlts-srtp-verification' hash setup>
     * with the profile's <encrypted/> inside, and the device id in XEP-0353 <proceed/>.
     *
     * The legacy profile carries the fingerprint as payload text, as those clients do; OMEMO 2,
     * chosen when the peer advertises it, carries the whole <fingerprint/> element.
     *
     * Sessions get Session::peerAuthentication(). For a session with
     * Session::isEndToEndRequired() the filter refuses unprotected fingerprints and transports
     * without DTLS, and fails when it cannot encrypt for the peer, with not-acceptable.
     */
    class IRIS_EXPORT OmemoDtlsVerification : public QObject, public SignalingFilter {
        Q_OBJECT
    public:
        static QString ns();

        /// Registers itself as a signaling filter of @a manager until destroyed.
        OmemoDtlsVerification(Manager *manager, OmemoEncryption *omemo, QObject *parent = nullptr);
        ~OmemoDtlsVerification() override;

        /// The peer device of a session: learned from <proceed/> or from its encrypted fingerprints.
        void                   setPeerDevice(const Jid &peer, const QString &sid, quint32 deviceId);
        std::optional<quint32> peerDevice(const Jid &peer, const QString &sid) const;

        bool filterIncoming(const Jid &from, const QDomElement &jingle, const Done &done) override;
        bool filterOutgoing(const Jid &to, const QDomElement &jingle, const Done &done) override;
        void decorateMessageInitiation(const Jid &to, MessageInitiation &initiation) override;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };
}
}

#endif // JINGLE_OMEMO_VERIFICATION_H
