/*
 * Copyright (C) 2026 Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef PSIMEDIA_SECURERTPCONTEXT_H
#define PSIMEDIA_SECURERTPCONTEXT_H

#include "psimediaprovider.h"

#include <QObject>
#include <memory>

namespace PsiMedia {

class SrtpAssociation {
public:
    SrtpAssociation();
    ~SrtpAssociation();

    SrtpAssociation(const SrtpAssociation &)            = delete;
    SrtpAssociation &operator=(const SrtpAssociation &) = delete;

    static QStringList supportedProfiles();

    bool configure(const QByteArray &associationId, quint64 epoch, const QString &profile,
                   const QByteArray &localMasterKey, const QByteArray &localMasterSalt,
                   const QByteArray &remoteMasterKey, const QByteArray &remoteMasterSalt);
    void invalidate(const QByteArray &associationId, quint64 epoch);
    void reset();

    bool                           isReady() const;
    QByteArray                     associationId() const;
    quint64                        epoch() const;
    SecureRtpSessionContext::Error lastError() const;

    bool protect(const PSecureRtpPacket &plain, PSecureRtpPacket *protectedPacket);
    bool unprotect(const PSecureRtpPacket &protectedPacket, PSecureRtpPacket *plain);

private:
    bool process(const PSecureRtpPacket &input, PSecureRtpPacket *output, bool sending);

    class Private;
    std::unique_ptr<Private> d;
};

} // namespace PsiMedia

#endif // PSIMEDIA_SECURERTPCONTEXT_H
