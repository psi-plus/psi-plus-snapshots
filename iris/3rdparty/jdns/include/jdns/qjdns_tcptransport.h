/*
 * qjdns_tcptransport.h - DNS-over-TCP transport for QJDns
 * Copyright (C) 2026 Psi contributors
 * Licensed under the MIT license (see COPYING).
 */
#ifndef QJDNS_TCPTRANSPORT_H
#define QJDNS_TCPTRANSPORT_H

#include "qjdns_querytransport.h"
#include <QNetworkProxy>

/**
 * DNS-over-TCP (RFC 7766) using an explicitly configured Qt socket proxy.
 *
 * The DNS server can be local or remote; the transport name describes the
 * protocol, not its network location. Pass QNetworkProxy::NoProxy explicitly
 * for direct TCP. A SOCKS5 proxy receives the DNS server hostname in CONNECT.
 *
 * Composes an internal QJDnsExternalTransport for request IDs, cancellation,
 * timeouts and returning unframed DNS packets to JDNS. The external backend
 * completion methods are NOT exposed in the TCP transport's public API. No UDP or system-DNS fallback.
 */
class JDNS_EXPORT QJDnsTcpTransport : public QJDnsQueryTransport
{
public:
    /**
     * @brief Construct a DNS-over-TCP transport using RFC 7766 framing.
     * @param server Resolver hostname or IP address. If using SOCKS5, the
     * proxy resolves this hostname for its CONNECT request. A loopback
     * address then refers to the proxy-side host, not the local machine.
     * @param port TCP port of the resolver (normally 53).
     * @param proxy Explicit proxy policy. Use QNetworkProxy::NoProxy for
     * a direct TCP connection or a SOCKS5 proxy for remote DNS routing.
     * @param parent Optional QObject owner; QJDns::create(transport, parent)
     * always takes ownership, including on failure.
     * @note Network connections are started per request, not on
     * construction. There is no silent fallback to UDP or local DNS.
     * When the endpoint is a hostname and NoProxy is selected, Qt may
     * resolve that endpoint using the system's name-resolution policy.
     */
    QJDnsTcpTransport(const QString &server, quint16 port,
                      const QNetworkProxy &proxy, QObject *parent = nullptr);

    /**
     * @brief Abort pending TCP requests and release owned sockets.
     * @note Cancellation prevents late responses from completing queries.
     */
    ~QJDnsTcpTransport() override;

    /**
     * @brief Set the maximum lifetime for subsequently started DNS requests.
     * @param milliseconds Positive timeout in milliseconds. Nonpositive
     * values are ignored; the default is 12000 ms.
     * @note An expired request aborts its TCP operation and reports Timeout
     * to JDNS. It never manufactures a DNS SERVFAIL or retries through
     * another resolver.
     */
    void setTimeout(int milliseconds);

    /**
     * @brief Begin a DNS wire query without fake socket handles/endpoints.
     * @param id Unique resolver request ID.
     * @param packet Complete DNS wire request, without transport framing.
     */
    void startRequest(int id, const QByteArray &packet) override;

    /**
     * @brief Cancel an unfinished DNS query.
     * @param id Resolver request ID, not the 16-bit DNS packet ID.
     * @note Safe after completion; late replies are ignored.
     */
    void cancelRequest(int id) override;

private:
    class Private;
    Private *d;
    void submitDns(int id, const QByteArray &packet);
    void cancelDns(int id);
    void finishDns(int id, const QByteArray &response, const QString &error);
};

#endif // QJDNS_TCPTRANSPORT_H
