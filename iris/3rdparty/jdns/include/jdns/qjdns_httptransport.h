/*
 * qjdns_httptransport.h - DNS-over-HTTPS transport for QJDns
 * Copyright (C) 2026 Psi contributors
 * Licensed under the MIT license (see COPYING).
 */
#ifndef QJDNS_HTTPTRANSPORT_H
#define QJDNS_HTTPTRANSPORT_H

#include "qjdns_querytransport.h"
#include <QUrl>

class QNetworkAccessManager;

/**
 * @brief DNS wire transport over HTTPS using Qt Network (RFC 8484).
 *
 * Despite the HTTP-oriented class name, **only HTTPS URLs are accepted**.
 * Plaintext HTTP is rejected before any network request is submitted.
 * This transport sends application/dns-message POST requests and requires
 * HTTP 200 and the matching response media type. No redirects are followed.
 *
 * If manager is null, a QNetworkAccessManager is created and owned by this
 * transport. Otherwise, the provided manager remains caller-owned and must
 * outlive active DNS requests. HTTP redirects are not followed.
 *
 * The internally created manager uses Qt's default networking and proxy
 * policy. To guarantee that DoH uses a particular SOCKS5/HTTP proxy, pass
 * an explicitly configured QNetworkAccessManager, or configure the manager
 * returned by networkAccessManager() before starting the first query.
 * No fallback to another DNS transport occurs when HTTPS fails.
 *
 * Composes an internal QJDnsExternalTransport for DNS transaction bookkeeping
 * and timeout without exposing its backend completion methods to callers.
 * For a non-QNAM HTTP/TLS stack (including QCA), use QJDnsExternalTransport
 * directly and perform HTTP requests in the caller.
 */
class JDNS_EXPORT QJDnsHttpTransport : public QJDnsQueryTransport
{
public:
    /**
     * @brief Create a DNS-over-HTTPS resolver transport with a fixed endpoint.
     * @param endpoint HTTPS URL for an RFC 8484 DNS resolver. Invalid,
     * non-HTTPS endpoints and URLs containing user information or fragments
     * fail a query without issuing a network request.
     * @param manager Optional QNetworkAccessManager. When omitted, a manager
     * is created and owned by this transport; when provided, ownership stays
     * with the caller, who must keep it alive during outstanding requests.
     * @param parent Optional QObject parent. QJDns assumes ownership of a
     * transport passed to create(), including on failure.
     * @warning An internally created manager follows Qt's normal network
     * configuration; it does not automatically inherit caller-specific proxy
     * policy. Configure networkAccessManager() before querying when routing
     * through a particular proxy is required. Unless you explicitly supply
     * a proxy or an already-resolved endpoint, Qt may resolve the HTTPS
     * endpoint hostname using the system resolver during connection setup.
     * @note This transport is asynchronous and has no fallback to UDP,
     * a different HTTPS service or local DNS after an HTTPS request fails.
     * Pass the transport to QJDns::create(transport, parent).
     */
    QJDnsHttpTransport(const QUrl &endpoint, QNetworkAccessManager *manager = nullptr,
                      QObject *parent = nullptr);
    /**
     * @brief Cancel active HTTP requests and dispose of owned resources.
     * @note A caller-supplied QNetworkAccessManager is never deleted by
     * this transport. An internally created QNetworkAccessManager is owned
     * by the transport and released with it.
     */
    ~QJDnsHttpTransport() override;

    /**
     * @brief Access the configured Qt HTTP backend.
     * @return Caller-supplied QNetworkAccessManager, or the manager created
     * and owned by this transport; nullptr if an external manager was
     * destroyed before this call.
     * @warning Do not delete, reparent or take ownership of the returned
     * pointer. Set proxy and TLS policy before starting any query; changing
     * policy afterward may alter the routing of subsequent requests.
     * An external manager must remain alive while requests are in progress.
     */
    QNetworkAccessManager *networkAccessManager() const;

    /**
     * @brief Configure per-query maximum lifetime for future requests.
     * @param milliseconds Positive timeout in milliseconds (default 12000).
     * Zero and negative values are ignored.
     * @note An expired HTTPS request is cancelled and a typed Timeout is
     * reported to JDNS. Neither HTTP redirect nor DNS fallback is attempted.
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

#endif // QJDNS_HTTPTRANSPORT_H
