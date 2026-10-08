// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_ICE_DTLS_P_H
#define JINGLE_ICE_DTLS_P_H

#include <QPointer>

namespace XMPP { namespace Jingle { namespace ICE {
    // QCA's datagram queue can contain multiple plaintext records behind one
    // readiness notification. Do not assume one record per provider update.
    template <typename Source, typename Sink> void forwardPendingDtlsDatagrams(Source *source, Sink *sink)
    {
        QPointer<Source> sourceGuard(source);
        QPointer<Sink>   sinkGuard(sink);
        while (sourceGuard && sinkGuard) {
            const auto data = sourceGuard->readDatagram();
            if (data.isEmpty() || !sinkGuard)
                return;
            sinkGuard->writeIncoming(data);
        }
    }
}}}

#endif
