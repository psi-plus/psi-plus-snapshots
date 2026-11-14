/*
 * qjdns_externaltransport.h - application-owned asynchronous DNS backend
 * SPDX-License-Identifier: MIT
 */
#ifndef QJDNS_EXTERNALTRANSPORT_H
#define QJDNS_EXTERNALTRANSPORT_H

#include "qjdns_querytransport.h"

/**
 * @brief Backend-neutral asynchronous DNS wire-query transport.
 *
 * JDNS builds the request, validates the response header and question, then
 * decodes and caches DNS resource records. This class performs no HTTP,
 * TLS, socket or proxy I/O itself. A caller-owned network backend must handle
 * every queryRequested() and eventually call completeRequest() or failRequest(),
 * unless the request is cancelled.
 *
 * Requests use internal resolver IDs, distinct from the 16-bit DNS header ID.
 * Queries are delivered asynchronously, so caller networking never reenters
 * the C resolver directly from startRequest().
 *
 * @warning A query with no connected backend will time out; there is no
 * automatic routing to system DNS, UDP or any other resolver.
 * @note Backend slots and transport methods must be invoked on the owning
 * QObject thread (use queued signals/invocations across threads).
 */
class JDNS_EXPORT QJDnsExternalTransport : public QJDnsQueryTransport
{
    Q_OBJECT
public:
    /**
     * @brief Construct an HTTP-, TLS-, and proxy-agnostic DNS backend adapter.
     * @param parent Optional QObject owner, superseded by QJDns::create(transport, parent),
     * which always takes ownership, including on failure.
     * @note Connect queryRequested() to an application-owned asynchronous
     * backend before issuing queries. Backend network resources are not
     * created or owned by this adapter.
     */
    explicit QJDnsExternalTransport(QObject *parent = nullptr);

    /**
     * @brief Cancel still-dispatched requests and free the adapter.
     * @note Calls requestCancelled() for outstanding dispatched requests;
     * the application must stop its own network operations and retains
     * ownership of its external network backend.
     */
    ~QJDnsExternalTransport() override;

    /**
     * @brief Configure the deadline for subsequently initiated DNS requests.
     * @param milliseconds Positive timeout in milliseconds. Zero and
     * negative values are ignored; default is 12000 ms.
     * @note An expired dispatched request emits requestCancelled(), then
     * reports failed(..., Timeout) to the DNS engine. No synthetic DNS
     * SERVFAIL packet or alternate resolver is used.
     * Already pending requests retain their previous deadline.
     */
    void setTimeout(int milliseconds);

    /**
     * @brief Accept and schedule one unframed wire query for external delivery.
     * @param id Unique internal resolver request ID, not a DNS header ID.
     * @param dnsPacket Complete DNS wire query, including the DNS header.
     * @note Valid requests emit queryRequested() on a later event-loop turn.
     * Invalid lengths produce a cancellable deferred NetworkError.
     * This method never opens a socket itself.
     */
    void startRequest(int id, const QByteArray &dnsPacket) override;

    /**
     * @brief Cancel and discard an outstanding external DNS operation.
     * @param id Request ID previously passed to startRequest().
     * @note If queryRequested() was already emitted, requestCancelled()
     * notifies the backend to abort network I/O. If cancelled before
     * dispatch, no queryRequested() or completion is emitted. Calls for
     * already finished or unknown IDs have no effect.
     */
    void cancelRequest(int id) override;

public slots:
    /**
     * @brief Return an unframed DNS response for a pending request.
     * @param id Resolver request ID from queryRequested().
     * @param dnsResponse Complete DNS wire message.
     * @note Late and duplicate results are ignored. Call on the QObject
     * thread; for other threads use a queued invocation.
     * JDNS validates the DNS transaction ID and question.
     */
    void completeRequest(int id, const QByteArray &dnsResponse);

    /**
     * @brief Report failure of an application-owned network operation.
     * @param id Resolver request ID from queryRequested().
     * @param reason Diagnostic string; no DNS query is retried locally.
     * @param kind NetworkError or Timeout.
     */
    void failRequest(int id, const QString &reason, Failure kind = NetworkError);

signals:
    /**
     * @brief Ask external code to perform asynchronous DNS network delivery.
     * @param id Internal wire-query ID, not the 16-bit DNS transaction ID.
     * @param dnsRequest Complete unframed DNS message. For an RFC 8484
     * backend, send as a POST with Content-Type: application/dns-message;
     * other network stacks and DNS-over-TLS backends are also supported.
     * @note Emitted on a later event-loop turn, never within startRequest().
     * The application backend owns its sockets/HTTP requests and must report
     * completion via completeRequest() or failRequest(), or abort on
     * requestCancelled(). Unanswered requests eventually time out.
     */
    void queryRequested(int id, const QByteArray &dnsRequest);

    /**
     * @brief Request cancellation of previously dispatched backend work.
     * @param id ID originally supplied by queryRequested().
     * @note Emitted for timeout, final-consumer cancellation, or destruction
     * after queryRequested(). The backend should abort its work. Late
     * completeRequest()/failRequest() calls for this ID are ignored.
     * Cancellation before queryRequested() emits no backend notification.
     */
    void requestCancelled(int id);

private:
    struct Private;
    Private *d;
    void forget(int id, bool notifyCancel);
};

#endif // QJDNS_EXTERNALTRANSPORT_H
