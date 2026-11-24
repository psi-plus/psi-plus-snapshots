/*
 * jingle-dtls-omemo.h - OMEMO-verified DTLS fingerprints in Jingle
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

#ifndef JINGLE_DTLS_OMEMO_H
#define JINGLE_DTLS_OMEMO_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-session.h>

#include <QObject>

#include <functional>
#include <memory>
#include <optional>

namespace XMPP {
class OmemoEncryption;

namespace Jingle {
    /**
     * OMEMO protection for DTLS fingerprints in Jingle ICE transports.
     *
     * The ICE transport owns parsing and preparation of its fingerprint;
     * this service only encrypts or verifies an individual fingerprint.
     * A verification job never starts DTLS or mutates the transport.
     */
    class IRIS_EXPORT OmemoDtlsVerification : public QObject {
        Q_OBJECT
    public:
        static QString ns();
        static QString directFeature();
        static QString methodId();

        OmemoDtlsVerification(Manager *manager, OmemoEncryption *omemo, QObject *parent = nullptr);
        ~OmemoDtlsVerification() override;

        void                   setPeerDevice(const Jid &peer, const QString &sid, quint32 deviceId);
        std::optional<quint32> peerDevice(const Jid &peer, const QString &sid) const;

        using FingerprintDone  = std::function<void(const QDomElement &fingerprint, const QString &error)>;
        using VerificationDone = std::function<void(
            const QDomElement &fingerprint, const Session::PeerAuthentication &authentication, const QString &error)>;
        void encryptFingerprint(Session *session, const QDomElement &fingerprint, const FingerprintDone &done,
                                bool allowUnselectedDevice = false);
        void decryptFingerprint(Session *session, const QDomElement &fingerprint, const VerificationDone &done);

        void decorateMessageInitiation(const Jid &to, MessageInitiation &initiation);

    private:
        class Private;
        std::unique_ptr<Private> d;
    };
}
}

#endif // JINGLE_DTLS_OMEMO_H
