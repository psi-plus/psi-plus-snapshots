/*
 * jingle-jet-omemo.h - OMEMO key delivery for JET (XEP-0396)
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef JINGLE_JET_OMEMO_H
#define JINGLE_JET_OMEMO_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-jet.h>

#include <QObject>
#include <memory>

namespace XMPP {
class OmemoEncryption;

namespace Jingle {

    /**
     * XEP-0396 profile of XEP-0391: OMEMO encrypts the JET transport secret.
     * Generic JET signaling, content association and streaming live in the JET registry, pad, protection and connection.
     * Legacy OMEMO uses its key-transport envelope (AES-128-GCM); OMEMO 2 uses
     * an SCE-wrapped secret (AES-256-GCM).
     */
    class IRIS_EXPORT JetOmemo : public QObject, public JET::KeyTransport {
        Q_OBJECT
    public:
        static QString ns();    // urn:xmpp:jingle:jet-omemo:0
        static QString jetNs(); // urn:xmpp:jingle:jet:0

        JetOmemo(Manager *manager, OmemoEncryption *omemo, QObject *parent = nullptr);
        ~JetOmemo() override;

        QString     methodId() const override;
        QStringList discoFeatures() const override;
        bool        canEncrypt(const Jid &peer) const override;
        bool        accepts(const QString &wireProfile) const override;
        void        encryptSecret(const Jid &peer, const Done &done) override;
        void        decryptSecret(const Jid &peer, const QDomElement &security, const Done &done) override;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };

}
} // namespace XMPP::Jingle
#endif // JINGLE_JET_OMEMO_H
