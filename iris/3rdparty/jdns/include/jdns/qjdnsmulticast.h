/*
 * Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef QJDNSMULTICAST_H
#define QJDNSMULTICAST_H

#include "qjdnsshared.h"
#include <QNetworkInterface>

class QJDnsMulticastPrivate;

/**
 * Multicast DNS on one explicitly selected network link (RFC 6762).
 *
 * Reuses the QJDnsShared engine, including probing, conflict detection,
 * caching, TTL expiry and goodbye packets. Requests are independent QObject
 * consumers, not shared QObject owners. All factory-returned requests start
 * on the event loop: even invalid input and cache hits cannot complete before
 * the caller connects resultsReady(). Use this object and its requests on the
 * same thread, with a running event loop.
 *
 * subscribe() is continuous: results() contains changes, not a full snapshot.
 * A zero TTL removes a record. publish() stays active after its first success
 * and can later report ErrorConflict. Deleting/cancelling a request stops that
 * consumer or withdraws its publication; it does not affect other consumers.
 *
 * One object represents one link. Keep the networkInterface() with discovered
 * endpoints: identical .local. names on different links are not interchangeable.
 * Address/interface changes require shutting down and recreating the object.
 * Call shutdown() and wait for shutdownFinished() before destruction to send
 * goodbyes. Destruction itself does not block or run a nested event loop.
 */
class JDNS_EXPORT QJDnsMulticast : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Create an IPv4/IPv6 mDNS session on a chosen interface.
     * @param interface Valid, up interface with at least one usable address.
     * @param parent Optional QObject owner.
     * @return Ready session, or nullptr when no multicast socket can be opened.
     * Only successfully opened address families are returned by addresses().
     * No routing through the default interface is substituted on failure.
     */
    static QJDnsMulticast *create(const QNetworkInterface &interface, QObject *parent = nullptr);

    /**
     * @brief Create a session for one IP family on its owning interface.
     * @param address Concrete local address currently assigned to an up interface.
     * @param parent Optional QObject owner.
     * @return Ready session, or nullptr for invalid/wildcard/non-local addresses.
     * Useful when the application explicitly selects IPv4 or IPv6.
     */
    static QJDnsMulticast *create(const QHostAddress &address, QObject *parent = nullptr);

    /** @brief Destroy the session; use shutdown() first to flush goodbyes. */
    ~QJDnsMulticast() override;

    /** @return The selected link, including its index for IPv6 scope IDs. */
    QNetworkInterface networkInterface() const;
    /** @return One bound source address per successfully opened IP family. */
    QList<QHostAddress> addresses() const;

    /**
     * @brief Build a DNS-SD instance name (RFC 6763), without lossy IDNA encoding.
     * @param instance Display name; NFC UTF-8, 1..63 octets, no ASCII controls.
     * @param serviceType Service and protocol, e.g. "_presence._tcp" (optionally
     * followed by ".local."). Other discovery domains are outside this API.
     * @return Escaped absolute name, or an empty QByteArray for invalid input.
     * Literal dots/backslashes in the instance remain inside its single label.
     */
    static QByteArray serviceName(const QString &instance, const QByteArray &serviceType);

    /**
     * @brief Extract the UTF-8 display label from a DNS-SD instance name.
     * @param name Absolute presentation name received in a PTR record.
     * @param serviceType Expected service type, as for serviceName().
     * @return Decoded instance, or an empty QString if invalid/wrong service.
     */
    static QString instanceName(const QByteArray &name, const QByteArray &serviceType);

    /**
     * @brief Subscribe to record changes until cancellation or destruction.
     * @param name Absolute DNS presentation name, e.g. "_presence._tcp.local.".
     * @param type DNS record type, e.g. QJDns::Ptr, Srv, Txt, A or Aaaa.
     * @param parent Request owner; nullptr makes the session its owner.
     * @return A pending, independently cancellable request (never nullptr).
     * Errors and initial cached records are reported asynchronously.
     */
    QJDnsSharedRequest *subscribe(const QByteArray &name, int type, QObject *parent = nullptr);

    /**
     * @brief Publish a record until cancellation or destruction.
     * @param record Absolute owner name, positive TTL and valid record data.
     * @param mode Unique probes for conflicts; Shared is normally used for PTR.
     * @param parent Request owner; nullptr makes the session its owner.
     * @return Pending request. First success confirms publication; later errors
     * may report conflicts. Use publishUpdate() to update it. An unspecified
     * A/AAAA address is filled from addresses(), as in QJDnsShared.
     */
    QJDnsSharedRequest *publish(const QJDns::Record &record, QJDns::PublishMode mode = QJDns::Unique,
                               QObject *parent = nullptr);

    /**
     * @brief Withdraw one of this session's publications (same as cancel()).
     * @param publication Request returned by this object's publish().
     * @return False for nullptr, another session's request, or a subscription.
     * The caller still owns the request and may delete it normally.
     */
    bool unpublish(QJDnsSharedRequest *publication);

    /** @brief Stop requests and send goodbye packets. Idempotent; cannot restart. */
    void shutdown();

signals:
    /** @brief All sockets have closed after sending queued goodbye records. */
    void shutdownFinished();

private:
    explicit QJDnsMulticast(QObject *parent);
    static QJDnsMulticast *createOn(const QNetworkInterface &interface,
                                   const QList<QHostAddress> &addresses, QObject *parent);
    QJDnsMulticastPrivate *d;
};

#endif
