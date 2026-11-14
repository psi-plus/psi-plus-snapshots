/*
 * Copyright (C) 2026  Psi contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef QJDNS_DATAGRAMTRANSPORT_H
#define QJDNS_DATAGRAMTRANSPORT_H

#include "jdns_export.h"

#include <QByteArray>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QObject>

/**
 * @brief Abstract packet transport used by QJDns.
 *
 * QJDns owns the DNS protocol state machine and record parsing; this interface
 * only moves complete DNS wire packets to and from the network.
 *
 * This private interface implements the built-in UDP and mDNS backends.
 * Applications select public query transports through QJDns::create().
 *
 * @note QJDns and its transport must be used on their QObject thread.
 * @note For asynchronous unicast DNS operations use QJDnsQueryTransport.
 * This datagram interface is for UDP/mDNS and explicitly carries socket
 * bind addresses and packet destination endpoints.
 */
class JDNS_EXPORT QJDnsDatagramTransport : public QObject
{
    Q_OBJECT
public:
    /** @brief Result of submitting a DNS wire packet to the transport. */
    enum SubmitResult {
        RetryLater, ///< Packet not accepted; JDNS may try delivery later.
        Dropped,    ///< Packet lost/failed synchronously; not queued by transport.
        Submitted   ///< Packet accepted; later signal packetWritten() is required.
    };

    /** @brief Complete incoming DNS wire packet and its source endpoint. */
    struct Packet
    {
        QHostAddress sourceAddress; ///< Source IP address, if available.
        quint16 sourcePort = 0;     ///< Source UDP port, if available.
        QByteArray data;            ///< Unframed DNS message, including header.
    };

    /**
     * @brief Construct a transport with an optional QObject parent.
     * @param parent Initial owner; replaced by QJDns after successful injection.
     */
    explicit QJDnsDatagramTransport(QObject *parent = nullptr) : QObject(parent) {}

    /** @brief Destroy the transport and release its platform resources. */
    ~QJDnsDatagramTransport() override = default;

    /**
     * @brief Open a transport handle on behalf of the DNS engine.
     * @param address Local bind/interface address requested by JDNS.
     * @param port Local port requested by JDNS.
     * @param multicastAddress Multicast group, if multicast mode is requested.
     * @return A nonzero transport handle on success, or 0 on failure.
     * @note This interface binds a real datagram socket. For logical DNS
     * requests without a socket, use QJDnsQueryTransport instead.
     */
    virtual int open(const QHostAddress &address, quint16 port,
                     const QHostAddress &multicastAddress) = 0;

    /**
     * @brief Close a previously opened handle.
     * @param handle Value returned by open().
     * @note Release resources and stop pending I/O for this handle.
     * Deferred or late network responses must not be delivered after close().
     */
    virtual void close(int handle) = 0;

    /**
     * @brief Submit one complete, unframed DNS wire request.
     * @param handle Open transport handle.
     * @param destination Target IP address supplied by the DNS engine.
     * @param port UDP destination port supplied by the DNS engine.
     * @param packet DNS message, including the DNS header; do not assume a
     * TCP length prefix or HTTP framing is present.
     * @return RetryLater, Dropped or Submitted (see SubmitResult).
     * @note After returning Submitted, acknowledge acceptance using
     * packetWritten(). Deliver accepted UDP datagrams via readyRead() and
     * takeResponse(). Application-level request failures belong to the
     * distinct QJDnsQueryTransport interface.
     * @warning Avoid synchronous callbacks into JDNS from inside submit();
     * schedule completion on the QObject event loop instead.
     */
    virtual SubmitResult submit(int handle, const QHostAddress &destination,
                                quint16 port, const QByteArray &packet) = 0;

    /**
     * @brief Consume one queued incoming DNS response.
     * @param handle Open transport handle.
     * @param packet Output, populated only when a response is available.
     * @return true when one packet was removed; false if none is available.
     * @note Do not include DNS-over-TCP length prefixes or HTTP envelopes.
     * Real datagrams provide the sender endpoint when available.
     */
    virtual bool takeResponse(int handle, Packet *packet) = 0;

signals:
    /**
     * @brief Notify QJDns that at least one packet can be consumed.
     * @param handle Handle with pending data for takeResponse().
     * @note Emit after queueing the packet, on the transport's Qt thread.
     */
    void readyRead(int handle);

    /**
     * @brief Acknowledge an accepted asynchronous packet submission.
     * @note Required after submit() returns Submitted; otherwise JDNS's
     * pending-write bookkeeping and shutdown may not make progress.
     */
    void packetWritten();

    /**
     * @brief Report diagnostic details to QJDns's debug line collector.
     * @param line Human-readable diagnostic text (not a wire packet).
     */
    void debugLine(const QString &line);
};

/**
 * @brief Construct the platform's default transport, usually UDP.
 * @param parent QObject owner of the created transport.
 * @return Newly allocated transport owned by @p parent.
 * @note This factory is used internally by QJDns when no custom transport
 * was injected. Applications should use QJDns::create() instead.
 */
QJDnsDatagramTransport *qjdns_create_datagram_transport(QObject *parent,
                                                    const QNetworkInterface &interface = QNetworkInterface());

#endif
