/* Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef XMPP_LINKLOCALSTREAM_H
#define XMPP_LINKLOCALSTREAM_H

#include "xmpp_stream.h"
#include <iris/jid/jid.h>

class QIODevice;

namespace XMPP {
/**
 * XEP-0174 XML message/IQ stream over an already connected byte transport.
 *
 * DNS-SD discovery, socket selection and presence policy belong to the caller.
 * This stream reuses the Iris XML parser and Stanza API. It does not perform
 * SASL or STARTTLS and never advertises them; a peer requiring STARTTLS is
 * rejected. The transport may be secured externally. In particular an ordinary
 * TCP transport is unauthenticated and unencrypted: do not treat an advertised
 * DNS name or a matching stream address as authenticated identity.
 *
 * Each stanza's addresses are checked against the stream endpoints to prevent
 * third-party stanza injection. XML DTDs/entities, unsolicited presence, oversized
 * input and repeated stream headers are rejected. The stream close handshake
 * accepts in-flight stanzas and mirrors the peer's closing tag before closing
 * the transport. All objects must live on the same Qt thread.
 */
class IRIS_EXPORT LinkLocalStream : public Stream {
    Q_OBJECT
public:
    enum class Direction { Initiator, Recipient };

    /**
     * Begin on the next event-loop turn, so signals can be connected first.
     * transport is caller-owned and must be open ReadWrite. local is a bare
     * user@machine JID. peer is required for an initiator; a recipient may leave
     * it empty and learn the peer address from its opening stream header.
     */
    LinkLocalStream(QIODevice *transport, const Jid &local, const Jid &peer, Direction direction,
                    QObject *parent = nullptr);
    ~LinkLocalStream() override;

    Jid                     localJid() const;
    Jid                     peerJid() const;
    bool                    isConnected() const;
    QDomDocument           &doc() const override;
    QString                 baseNS() const override;
    bool                    old() const override;
    void                    close() override;
    bool                    stanzaAvailable() const override;
    Stanza                  read() override;
    void                    write(const Stanza &stanza) override;
    int                     errorCondition() const override;
    QString                 errorText() const override;
    QHash<QString, QString> errorLangText() const override;
    QDomElement             errorAppSpec() const override;

signals:
    /** Headers/features accepted; message and IQ stanzas may now be sent. */
    void connected();

private:
    class Private;
    Private *d;
};
}
#endif
