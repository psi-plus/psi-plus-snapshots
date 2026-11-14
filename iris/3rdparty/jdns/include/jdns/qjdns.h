/*
 * Copyright (C) 2005,2006  Justin Karneges
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

/**
 * @file
 *
 * The Qt wrapper to jdns.
 */

#ifndef QJDNS_H
#define QJDNS_H

#include "jdns_export.h"
#include <QtCore>
#include <QtNetwork>
#include <cstddef>

class QJDnsQueryTransport;
class QJDnsSharedRequest;

/**
 * @brief Low-level, asynchronous Qt wrapper around the JDNS DNS engine.
 *
 * Use create() for an initialized unicast resolver using system DNS,
 * DNS-over-TCP, DNS-over-HTTPS or an application-owned network backend.
 * The constructor and init(Mode, QHostAddress) remain available for explicit
 * UDP configuration and multicast DNS.
 *
 * JDNS still encodes/decodes DNS messages; transport injection changes only
 * the network I/O and the semantics of DNS server selection. The caller owns
 * QJDns, while create(transport, parent) always takes transport ownership,
 * including on initialization failure.
 *
 * @note QJDns is QObject/event-loop based. Keep the instance and its
 * transport on the same Qt thread.
 * @warning A query transport does not automatically inherit another
 * network component's proxy configuration; configure its endpoint explicitly.
 * @see QJDnsSharedRequest, QJDnsTcpTransport, QJDnsHttpTransport,
 * QJDnsExternalTransport
 */
class JDNS_EXPORT QJDns : public QObject
{
    Q_OBJECT
public:
    /** @brief DNS session mode; query transports support Unicast only. */
    enum Mode
    {
        Unicast,
        Multicast
    };

    /** @brief Ownership semantics when publishing multicast DNS records. */
    enum PublishMode
    {
        Unique,
        Shared
    };

    /** @brief Common DNS RR type codes as defined by the DNS protocol. */
    enum Type
    {
        A       = 1,
        Aaaa    = 28,
        Mx      = 15,
        Srv     = 33,
        Cname   = 5,
        Ptr     = 12,
        Txt     = 16,
        Hinfo   = 13,
        Ns      = 2,
        Any     = 255
    };

    /** @brief Errors emitted for DNS queries or multicast publication. */
    enum Error
    {
        ErrorGeneric,
        ErrorNXDomain, ///< query only
        ErrorTimeout,  ///< query only
        ErrorConflict  ///< publish only
    };

    class JDNS_EXPORT NameServer
    {
    public:
        QHostAddress address;
        int port;

        /** @brief Initialize a nameserver endpoint with default port 53. */
        NameServer();
    };

    class JDNS_EXPORT DnsHost
    {
    public:
        QByteArray name;
        QHostAddress address;
    };

    class JDNS_EXPORT SystemInfo
    {
    public:
        QList<NameServer> nameServers;
        QList<QByteArray> domains;
        QList<DnsHost> hosts;
    };

    class JDNS_EXPORT Record
    {
    public:
        QByteArray owner;
        int ttl;
        int type;
        QByteArray rdata;
        bool haveKnown;

        // known
        QHostAddress address;    ///< for A, Aaaa
        QByteArray name;         ///< for Mx, Srv, Cname, Ptr, Ns
        int priority;            ///< for Mx, Srv
        int weight;              ///< for Srv
        int port;                ///< for Srv
        QList<QByteArray> texts; ///< for Txt
        QByteArray cpu;          ///< for Hinfo
        QByteArray os;           ///< for Hinfo

        /** @brief Initialize an empty DNS resource record. */
        Record();

        /**
         * @brief Check whether this record can be encoded for publication.
         * @return true when its type/data satisfy JDNS record validation.
         */
        bool verify() const;
    };

    class JDNS_EXPORT Response
    {
    public:
        QList<Record> answerRecords;
        QList<Record> authorityRecords;
        QList<Record> additionalRecords;
    };

    /**
     * @brief Construct a DNS session.
     * @param parent Optional QObject owner.
     */
    QJDns(QObject *parent = 0);

    /** @brief Destroy the DNS session and its owned transport. */
    ~QJDns();

    /**
     * @brief Create an initialized resolver using the system DNS configuration.
     * @param parent Optional QObject owner.
     * @return Ready-to-query resolver, or nullptr on initialization failure.
     * @note Uses UDP on desktop and the platform resolver on Android.
     */
    static QJDns *create(QObject *parent = nullptr);

    /** @brief Disambiguate an explicit null parent from a null transport. */
    static QJDns *create(std::nullptr_t) { return create(); }

    /**
     * @brief Create a TCP or HTTPS resolver using Qt's application proxy policy.
     * @param server HTTPS resolver URL, or tcp://host[:port] (default port 53).
     * @param parent Optional QObject owner.
     * @return Initialized resolver, or nullptr for an invalid URL/init failure.
     */
    static QJDns *create(const QUrl &server, QObject *parent = nullptr);

    /**
     * @brief Disambiguate a null owner from a null HTTP manager.
     * @param server HTTPS or TCP resolver URL.
     */
    static QJDns *create(const QUrl &server, std::nullptr_t)
    { return create(server, static_cast<QObject *>(nullptr)); }

    /**
     * @brief Create a TCP or HTTPS resolver with an explicit proxy policy.
     * @param server HTTPS or TCP resolver URL.
     * @param proxy Qt proxy policy, including DefaultProxy or NoProxy.
     * @param parent Optional QObject owner.
     * @return Initialized resolver, or nullptr for an invalid URL/init failure.
     */
    static QJDns *create(const QUrl &server, const QNetworkProxy &proxy, QObject *parent = nullptr);

    /**
     * @brief Create an HTTPS resolver using a caller-owned HTTP manager.
     * @param server HTTPS resolver URL.
     * @param manager HTTP manager, or nullptr to create an internally owned one.
     * @param parent Optional QObject owner.
     * @return Initialized resolver, or nullptr for an invalid URL/init failure.
     * @note A supplied manager is never deleted and must outlive DNS requests.
     */
    static QJDns *create(const QUrl &server, QNetworkAccessManager *manager, QObject *parent = nullptr);

    /**
     * @brief Create an initialized resolver using an asynchronous query transport.
     * @param transport Heap-allocated transport on this thread. Ownership is
     * always transferred, including on failure. Do not share the object.
     * @param parent Optional QObject owner.
     * @return Initialized resolver, or nullptr for a null transport/init failure.
     */
    static QJDns *create(QJDnsQueryTransport *transport, QObject *parent = nullptr);

    /**
     * @brief Initialize the DNS engine and chosen network transport.
     * @param mode Unicast resolution or multicast querying/publishing.
     * @param address Network interface/bind address used by UDP/mDNS. Native
     * query transports do not bind; use create() to initialize them.
     * @return true when initialization succeeds; false otherwise.
     * @note init() may delete a configured transport if initialization fails;
     * do not retain or dereference a non-owning pointer to it afterward.
     * Query transports are rejected when @p mode is Multicast.
     */
    bool init(Mode mode, const QHostAddress &address);

    /**
     * @brief Start orderly asynchronous shutdown of an initialized session.
     * @note Handles and pending requests are retired by the DNS engine.
     * Wait for shutdownFinished() when the caller needs clean termination.
     * Does not select or fall back to another resolver.
     */
    void shutdown();

    /**
     * @brief Drain accumulated diagnostic messages.
     * @return Collected messages; reading clears the current backlog.
     * @see debugLinesReady()
     */
    QStringList debugLines();

    /**
     * @brief Read host OS DNS configuration.
     * @return Configured resolver addresses, search domains and hosts.
     * @warning This consults local system DNS *configuration*. It is not
     * required for proxied query transport use; callers seeking to avoid
     * local DNS leaks should not use it to select a fallback resolver.
     */
    static SystemInfo systemInfo();

    /**
     * @brief Detect a primary network interface suitable for multicast.
     * @param address IPv4/IPv6 address family or desired interface.
     * @return Interface address, or a null QHostAddress if detection fails.
     * @note This method probes multicast networking; it is unrelated to
     * asynchronous unicast query transports.
     */
    static QHostAddress detectPrimaryMulticast(const QHostAddress &address);

    /**
     * @brief Set explicit resolver addresses for normal unicast DNS.
     * @param list DNS servers that ordinary JDNS unicast packets may target.
     * @return true when the list was accepted. Returns false for native
     * query transports, where resolvers are configured on the transport, or
     * when called before initialization.
     */
    bool setNameServers(const QList<NameServer> &list);

    /**
     * @brief Create an independently cancellable, asynchronous unicast query.
     * @param name DNS name to query, in wire-compatible encoding.
     * @param type DNS record type, such as QJDns::Srv.
     * @param parent Request owner; defaults to this resolver when null.
     * @return A pending QJDnsSharedRequest; connect to resultsReady() immediately.
     * @note Start and completion are deferred until the event loop runs, even
     * for cache hits and validation errors. Deletion before start prevents I/O.
     * Results remain readable until the request is deleted. Identical queries
     * share network work, but cancellation/deletion affects only this consumer.
     * Use the request and resolver on the same QObject thread.
     */
    QJDnsSharedRequest *query(const QByteArray &name, int type, QObject *parent = nullptr);

    /**
     * @brief Start an asynchronous DNS resource record lookup.
     * @param name DNS name to query; use wire-compatible encoding.
     * @param type DNS record type, such as QJDns::Srv or QJDns::A.
     * @return JDNS request identifier; use it for resultsReady(), error(),
     * and queryCancel(). Negative values indicate failure; zero is valid.
     * @note The query uses only this instance's selected transport. For a
     * native query transport, completion and timeouts originate from that
     * transport; no implicit system-DNS retry is performed.
     */
    int queryStart(const QByteArray &name, int type);

    /**
     * @brief Cancel a previously started query.
     * @param id Identifier returned by queryStart().
     * @note For native query transports, JDNS cancels the underlying
     * network operation immediately after the final interested consumer
     * cancels. A shared query remains active while any consumer needs it.
     * Traditional UDP behavior may retain inactive transactions for caching.
     */
    void queryCancel(int id);

    /**
     * @brief Begin publishing a DNS resource record via multicast DNS.
     * @param m Unique or Shared publication semantics.
     * @param record DNS resource record to publish.
     * @return Publication handle used for updates, cancellation and signals.
     * @warning Multicast mode only; unsupported by unicast TCP, DoH and
     * External query transports.
     */
    int publishStart(PublishMode m, const Record &record);

    /**
     * @brief Replace the value of an active multicast publication.
     * @param id Publication handle from publishStart().
     * @param record Replacement resource record.
     * @note Has no meaning for ordinary unicast DNS resolution.
     */
    void publishUpdate(int id, const Record &record);

    /**
     * @brief Stop publishing a multicast DNS record.
     * @param id Publication handle from publishStart().
     */
    void publishCancel(int id);

signals:
    /**
     * @brief A DNS query has returned resource records.
     * @param id Identifier returned by queryStart().
     * @param results Answer, authority and additional record sections.
     * @note Filter answerRecords by the desired DNS type: CNAMEs and other
     * related records may accompany the requested RR type.
     */
    void resultsReady(int id, const QJDns::Response &results);

    /**
     * @brief A multicast publication has been acknowledged.
     * @param id Publication handle from publishStart().
     */
    void published(int id);

    /**
     * @brief A query or multicast publication failed.
     * @param id Operation handle from queryStart()/publishStart().
     * @param e DNS error category; consult debugLines() for diagnostics.
     * @note Native query transport failures are reported directly through
     * this signal; no synthetic DNS SERVFAIL or local fallback is used.
     */
    void error(int id, QJDns::Error e);

    /** @brief Orderly asynchronous shutdown is complete. */
    void shutdownFinished();

    /** @brief New messages are available via debugLines(). */
    void debugLinesReady();

private:
    class Private;
    friend class Private;
    friend class QJDnsSharedPrivate;
    Private *d;
};

#endif
