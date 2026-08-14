/*
 * xmpp_sce.h - XEP-0420 Stanza Content Encryption helper
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XMPP_SCE_H
#define XMPP_SCE_H

#include <QDomDocument>
#include <QFlags>
#include <QString>

#include <optional>

namespace XMPP {

/** Generic XEP-0420 stanza-content container used by E2EE methods. */
class StanzaContentEncryption {
public:
    enum Affix {
        NoAffixes     = 0x00,
        RandomPadding = 0x01,
        Time          = 0x02,
        To            = 0x04,
        From          = 0x08,
    };
    Q_DECLARE_FLAGS(Affixes, Affix)

    /**
     * Policy selected by an encryption profile.
     *
     * XEP-0420 defines the random part of rpad as 0..200 characters, but
     * deliberately leaves the fixed minimum stanza size to implementations.
     */
    struct Profile {
        Affixes affixes                 = {};
        int     minimumEnvelopeSize     = 0;
        int     maximumRandomPadding    = 200;
        int     maximumClockSkewSeconds = 0; // 0 = do not enforce Time freshness.
    };

    /** Owns both XML trees so returned QDomElements stay valid. */
    struct Prepared {
        QDomDocument outerDocument;
        QDomDocument envelopeDocument;

        QDomElement outer() const { return outerDocument.documentElement(); }
        QDomElement envelope() const { return envelopeDocument.documentElement(); }
        QByteArray  envelopeBytes() const { return envelopeDocument.toByteArray(-1); }
    };

    static QString namespaceUri();

    /**
     * Split a stanza into a plaintext server-processed shell and an SCE
     * envelope containing all sensitive direct child elements.
     */
    static std::optional<Prepared> prepare(const QDomElement &stanza, const Profile &profile, QString *error = nullptr);

    /**
     * Restore the application-visible stanza after decryption.
     *
     * Server-processed elements from encrypted content are discarded, while
     * sensitive plaintext extensions outside the encrypted content are ignored.
     */
    static std::optional<QDomDocument> restore(const QDomElement &encryptedOuter, const QDomElement &envelope,
                                               const Profile &profile, QString *error = nullptr);

    /** Known XEP-0420 server-only elements. Kept public for future registrar extensions. */
    static bool isServerProcessedElement(const QDomElement &element);
};

Q_DECLARE_OPERATORS_FOR_FLAGS(StanzaContentEncryption::Affixes)

} // namespace XMPP

#endif // XMPP_SCE_H
