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
 */

#ifndef JDNS_H
#define JDNS_H

#include "jdns_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*jdns_object_dtor_func)(void *);
typedef void *(*jdns_object_cctor_func)(const void *);

#define JDNS_OBJECT \
    jdns_object_dtor_func dtor; \
    jdns_object_cctor_func cctor;

/* Each generic vtable slot must hold a function with the exact prototype.
 * Casting a typed copy/destructor function pointer and invoking it through
 * an incompatible type is undefined behavior (and caught by UBSan). */
#define JDNS_OBJECT_NEW(name) \
    (name##_t *)jdns_object_new(sizeof(name##_t), \
        name##_delete_generic, name##_copy_generic);

typedef struct jdns_object
{
    JDNS_OBJECT
} jdns_object_t;

JDNS_EXPORT void *jdns_object_new(int size, void (*dtor)(void *),
                                  void *(*cctor)(const void *));
JDNS_EXPORT void *jdns_object_copy(const void *a);
JDNS_EXPORT void jdns_object_delete(void *a);
JDNS_EXPORT void jdns_object_free(void *a);

#define JDNS_LIST_DECLARE(name) \
    JDNS_OBJECT \
    int count; \
    name##_t **item;

typedef struct jdns_list
{
    JDNS_OBJECT
    int count;
    void **item;
    int valueList;
    int autoDelete;
} jdns_list_t;

JDNS_EXPORT jdns_list_t *jdns_list_new();
JDNS_EXPORT jdns_list_t *jdns_list_copy(const jdns_list_t *a);
JDNS_EXPORT void jdns_list_delete(jdns_list_t *a);
JDNS_EXPORT void jdns_list_clear(jdns_list_t *a);
JDNS_EXPORT void jdns_list_insert(jdns_list_t *a, void *item, int pos);
JDNS_EXPORT void jdns_list_insert_value(jdns_list_t *a, const void *item, int pos);
JDNS_EXPORT void jdns_list_remove(jdns_list_t *a, void *item);
JDNS_EXPORT void jdns_list_remove_at(jdns_list_t *a, int pos);

typedef struct jdns_string
{
    JDNS_OBJECT
    unsigned char *data;
    int size;
} jdns_string_t;

JDNS_EXPORT jdns_string_t *jdns_string_new();
JDNS_EXPORT jdns_string_t *jdns_string_copy(const jdns_string_t *s);
JDNS_EXPORT void jdns_string_delete(jdns_string_t *s);
JDNS_EXPORT void jdns_string_set(jdns_string_t *s, const unsigned char *str,
                                 int str_len);
JDNS_EXPORT void jdns_string_set_cstr(jdns_string_t *s, const char *str);

 // overlays jdns_list
typedef struct jdns_stringlist
{
    JDNS_OBJECT
    int count;
    jdns_string_t **item;
} jdns_stringlist_t;

JDNS_EXPORT jdns_stringlist_t *jdns_stringlist_new();
JDNS_EXPORT jdns_stringlist_t *jdns_stringlist_copy(const jdns_stringlist_t *a);
JDNS_EXPORT void jdns_stringlist_delete(jdns_stringlist_t *a);
JDNS_EXPORT void jdns_stringlist_append(jdns_stringlist_t *a, const jdns_string_t *str);

typedef struct jdns_address
{
    int isIpv6;
    union
    {
        unsigned long int v4;
        unsigned char *v6; // 16 bytes
    } addr;
    char *c_str;
} jdns_address_t;

JDNS_EXPORT jdns_address_t *jdns_address_new();
JDNS_EXPORT jdns_address_t *jdns_address_copy(const jdns_address_t *a);
JDNS_EXPORT void jdns_address_delete(jdns_address_t *a);
JDNS_EXPORT void jdns_address_set_ipv4(jdns_address_t *a, unsigned long int ipv4);
JDNS_EXPORT void jdns_address_set_ipv6(jdns_address_t *a, const unsigned char *ipv6);

/**
 * @return 1 if string was ok, else 0.  Note: IPv4 addresses only!
 */
JDNS_EXPORT int jdns_address_set_cstr(jdns_address_t *a, const char *str);

/**
 * @return 1 if the same, else 0
 */
JDNS_EXPORT int jdns_address_cmp(const jdns_address_t *a, const jdns_address_t *b);

// convenient predefined addresses/ports
#define JDNS_UNICAST_PORT    53
#define JDNS_MULTICAST_PORT  5353
JDNS_EXPORT jdns_address_t *jdns_address_multicast4_new(); // 224.0.0.251
JDNS_EXPORT jdns_address_t *jdns_address_multicast6_new(); // FF02::FB

typedef struct jdns_server
{
    unsigned char *name;
    /**
     *  SRV only
     */
    int port;
    int priority;

    /**
     * SRV only
     */
    int weight;
} jdns_server_t;

JDNS_EXPORT jdns_server_t *jdns_server_new();
JDNS_EXPORT jdns_server_t *jdns_server_copy(const jdns_server_t *s);
JDNS_EXPORT void jdns_server_delete(jdns_server_t *s);
JDNS_EXPORT void jdns_server_set_name(jdns_server_t *s, const unsigned char *name);

typedef struct jdns_nameserver
{
    jdns_address_t *address;
    int port;
} jdns_nameserver_t;

JDNS_EXPORT jdns_nameserver_t *jdns_nameserver_new();
JDNS_EXPORT jdns_nameserver_t *jdns_nameserver_copy(const jdns_nameserver_t *a);
JDNS_EXPORT void jdns_nameserver_delete(jdns_nameserver_t *a);
JDNS_EXPORT void jdns_nameserver_set(jdns_nameserver_t *a, const jdns_address_t *addr,
                                     int port);

typedef struct jdns_nameserverlist
{
    int count;
    jdns_nameserver_t **item;
} jdns_nameserverlist_t;

JDNS_EXPORT jdns_nameserverlist_t *jdns_nameserverlist_new();
JDNS_EXPORT jdns_nameserverlist_t *jdns_nameserverlist_copy(const jdns_nameserverlist_t *a);
JDNS_EXPORT void jdns_nameserverlist_delete(jdns_nameserverlist_t *a);
JDNS_EXPORT void jdns_nameserverlist_append(jdns_nameserverlist_t *a,
                                            const jdns_address_t *addr, int port);

typedef struct jdns_dnshost
{
    jdns_string_t *name;
    jdns_address_t *address;
} jdns_dnshost_t;

typedef struct jdns_dnshostlist
{
    int count;
    jdns_dnshost_t **item;
} jdns_dnshostlist_t;

typedef struct jdns_dnsparams
{
    jdns_nameserverlist_t *nameservers;
    jdns_stringlist_t *domains;
    jdns_dnshostlist_t *hosts;
} jdns_dnsparams_t;

JDNS_EXPORT jdns_dnsparams_t *jdns_dnsparams_new();
JDNS_EXPORT jdns_dnsparams_t *jdns_dnsparams_copy(jdns_dnsparams_t *a);
JDNS_EXPORT void jdns_dnsparams_delete(jdns_dnsparams_t *a);
JDNS_EXPORT void jdns_dnsparams_append_nameserver(jdns_dnsparams_t *a,
    const jdns_address_t *addr, int port);
JDNS_EXPORT void jdns_dnsparams_append_domain(jdns_dnsparams_t *a,
    const jdns_string_t *domain);
JDNS_EXPORT void jdns_dnsparams_append_host(jdns_dnsparams_t *a,
    const jdns_string_t *name, const jdns_address_t *address);

#define JDNS_RTYPE_A         1
#define JDNS_RTYPE_AAAA     28
#define JDNS_RTYPE_MX       15
#define JDNS_RTYPE_SRV      33
#define JDNS_RTYPE_CNAME     5
#define JDNS_RTYPE_PTR      12
#define JDNS_RTYPE_TXT      16
#define JDNS_RTYPE_HINFO    13
#define JDNS_RTYPE_NS        2
#define JDNS_RTYPE_ANY     255

typedef struct jdns_rr
{
    unsigned char *owner;
    int ttl;
    int type;
    int qclass;
    int rdlength;
    unsigned char *rdata;
    int haveKnown;

    union
    {
        jdns_address_t *address;  /**< for A, AAAA        */
        jdns_server_t *server;    /**< for MX, SRV        */
        unsigned char *name;      /**< for CNAME, PTR, NS */
        jdns_stringlist_t *texts; /**< for TXT            */
        struct
        {
            jdns_string_t *cpu;
            jdns_string_t *os;
        } hinfo; // for HINFO
    } data;
} jdns_rr_t;

JDNS_EXPORT jdns_rr_t *jdns_rr_new();
JDNS_EXPORT jdns_rr_t *jdns_rr_copy(const jdns_rr_t *r);
JDNS_EXPORT void jdns_rr_delete(jdns_rr_t *r);
JDNS_EXPORT void jdns_rr_set_owner(jdns_rr_t *r, const unsigned char *name);
JDNS_EXPORT void jdns_rr_set_record(jdns_rr_t *r, int type, const unsigned char *rdata,
    int rdlength);
JDNS_EXPORT void jdns_rr_set_A(jdns_rr_t *r, const jdns_address_t *address);
JDNS_EXPORT void jdns_rr_set_AAAA(jdns_rr_t *r, const jdns_address_t *address);
JDNS_EXPORT void jdns_rr_set_MX(jdns_rr_t *r, const unsigned char *name, int priority);
JDNS_EXPORT void jdns_rr_set_SRV(jdns_rr_t *r, const unsigned char *name, int port,
    int priority, int weight);
JDNS_EXPORT void jdns_rr_set_CNAME(jdns_rr_t *r, const unsigned char *name);
JDNS_EXPORT void jdns_rr_set_PTR(jdns_rr_t *r, const unsigned char *name);
JDNS_EXPORT void jdns_rr_set_TXT(jdns_rr_t *r, const jdns_stringlist_t *texts);
JDNS_EXPORT void jdns_rr_set_HINFO(jdns_rr_t *r, const jdns_string_t *cpu,
    const jdns_string_t *os);
JDNS_EXPORT void jdns_rr_set_NS(jdns_rr_t *r, const unsigned char *name);
// note: only works on known types
JDNS_EXPORT int jdns_rr_verify(const jdns_rr_t *r);

typedef struct jdns_response
{
    int answerCount;
    jdns_rr_t **answerRecords;
    int authorityCount;
    jdns_rr_t **authorityRecords;
    int additionalCount;
    jdns_rr_t **additionalRecords;
} jdns_response_t;

JDNS_EXPORT jdns_response_t *jdns_response_new();
JDNS_EXPORT jdns_response_t *jdns_response_copy(const jdns_response_t *r);
JDNS_EXPORT void jdns_response_delete(jdns_response_t *r);
JDNS_EXPORT void jdns_response_append_answer(jdns_response_t *r, const jdns_rr_t *rr);
JDNS_EXPORT void jdns_response_append_authority(jdns_response_t *r, const jdns_rr_t *rr);
JDNS_EXPORT void jdns_response_append_additional(jdns_response_t *r,
    const jdns_rr_t *rr);

#define JDNS_PUBLISH_SHARED   0x0001
#define JDNS_PUBLISH_UNIQUE   0x0002

#define JDNS_STEP_TIMER       0x0001
#define JDNS_STEP_HANDLE      0x0002

#define JDNS_EVENT_RESPONSE   0x0001
#define JDNS_EVENT_PUBLISH    0x0002
#define JDNS_EVENT_SHUTDOWN   0x0003

#define JDNS_STATUS_SUCCESS   0x0001
#define JDNS_STATUS_NXDOMAIN  0x0002
#define JDNS_STATUS_ERROR     0x0003
#define JDNS_STATUS_TIMEOUT   0x0004
#define JDNS_STATUS_CONFLICT  0x0005

typedef struct jdns_session jdns_session_t;

/**
 * @brief Resolver-wide clock, entropy and diagnostics.
 *
 * These callbacks are common to native asynchronous unicast DNS and real
 * datagram I/O. Their @c app argument is the pointer supplied below.
 * Transport-specific callbacks are configured separately when initializing
 * a datagram or asynchronous unicast session.
 */
typedef struct jdns_callbacks
{
    /** @brief User-supplied context forwarded unchanged to every callback. */
    void *app;

    /**
     * @brief Obtain the current resolver monotonic clock.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @return Milliseconds since session start.
     * @note Use a monotonic source; wall-clock adjustments can break timers.
     */
    int (*time_now)(jdns_session_t *s, void *app);

    /**
     * @brief Obtain pseudorandomness for DNS IDs and retry scheduling.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @return Random integer in the range 0..65535.
     */
    int (*rand_int)(jdns_session_t *s, void *app);

    /**
     * @brief Receive one diagnostic line from the DNS engine.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param line One diagnostic line. Treat its pointer as borrowed.
     */
    void (*debug_line)(jdns_session_t *s, void *app, const char *line);
} jdns_callbacks_t;

/**
 * @brief Socket-oriented datagram callbacks for UDP unicast and mDNS.
 *
 * In multicast mode, the bound socket needs to coexist with other mDNS
 * implementations (SO_REUSEPORT, where available), join the multicast group
 * (IP_ADD_MEMBERSHIP or IPv6 equivalent) and use a multicast TTL/hop limit of
 * 255. These requirements are transport responsibilities.
 *
 * Callbacks use the common jdns_callbacks_t::app context; the resolver
 * retains the transport handle returned by transport_bind() until unbind.
 * Do not use these callbacks with native asynchronous query transports.
 */
typedef struct jdns_datagram_callbacks
{
    /**
     * @brief Bind a UDP socket/interface on behalf of JDNS.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param address Local interface/bind address; NULL means all interfaces.
     * @param port Local UDP port; zero requests an ephemeral port.
     * @param multicast_address Multicast group to join, or NULL for unicast.
     * @return Positive transport handle on success, zero on failure.
     * @note For mDNS, enable socket reuse, join the multicast group and set
     * the multicast TTL/hop limit to 255 before delivering datagrams.
     */
    int (*transport_bind)(jdns_session_t *s, void *app,
        const jdns_address_t *address, int port,
        const jdns_address_t *multicast_address);

    /**
     * @brief Close a previously bound UDP transport handle.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param handle Positive handle returned by transport_bind().
     */
    void (*transport_unbind)(jdns_session_t *s, void *app, int handle);

    /**
     * @brief Receive a UDP packet with its source address and port.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param handle Positive handle returned by transport_bind().
     * @param sender Output for the sender's IP address.
     * @param sender_port Output for the sender's UDP port.
     * @param buffer Output buffer for the unframed DNS wire packet.
     * @param buffer_size In: available buffer capacity; out: bytes received.
     * @return 1 if a packet was read; 0 if none is available.
     * @note On a successful read, set both sender outputs and buffer_size.
     */
    int (*transport_read)(jdns_session_t *s, void *app, int handle,
        jdns_address_t *sender, int *sender_port, unsigned char *buffer,
        int *buffer_size);

    /**
     * @brief Submit a UDP DNS packet to its destination endpoint.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param handle Positive handle returned by transport_bind().
     * @param recipient IP address to which the DNS packet is sent.
     * @param recipient_port UDP destination port.
     * @param packet Complete unframed DNS wire packet.
     * @param packet_size Packet size in bytes.
     * @return 1 if the packet was accepted for writing, 0 if writing
     * should be retried later.
     * @note In asynchronous implementations the caller must notify JDNS
     * when a submitted packet has been written.
     */
    int (*transport_write)(jdns_session_t *s, void *app, int handle,
        const jdns_address_t *recipient, int recipient_port,
        unsigned char *packet, int packet_size);
} jdns_datagram_callbacks_t;

/**
 * @brief Asynchronous unicast request transport callbacks.
 * The transport selects endpoint, retry and timeout policy and sends full
 * DNS wire packets. No socket handles or datagram endpoints are exposed.
 */
typedef struct jdns_async_callbacks
{
    /**
     * @brief Start a query on an application network transport.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param id Internal wire-query ID (not the 16-bit DNS packet ID).
     * @param packet Unframed DNS query; borrowed for this callback only.
     * @param size Query length.
     * @warning Do not call jdns_async_complete() or jdns_async_fail()
     * reentrantly from this callback. Queue completion asynchronously.
     */
    void (*send)(jdns_session_t *s, void *app, int id,
                 const unsigned char *packet, int size);

    /**
     * @brief Cancel a pending network operation.
     * @param s Resolver session.
     * @param app User-supplied context from jdns_callbacks_t::app.
     * @param id Previously submitted wire-query ID.
     * @note Late completion is ignored and no acknowledgement is required.
     */
    void (*cancel)(jdns_session_t *s, void *app, int id);
} jdns_async_callbacks_t;

typedef struct jdns_event
{
    /**
     * JDNS_EVENT
     */
    int type;

    /**
     * query id or publish id
     */
    int id;

    /**
     * for query, this can be SUCCESS, NXDOMAIN, ERROR, or TIMEOUT
     * for publish, this can be SUCCESS, ERROR, or CONFLICT
     */
    int status;

    /**
     * for query
     */
    jdns_response_t *response;
} jdns_event_t;

JDNS_EXPORT void jdns_event_delete(jdns_event_t *e);

/**
 * @param callbacks the struct of callbacks
 * @return newly allocated session
 */
JDNS_EXPORT jdns_session_t *jdns_session_new(jdns_callbacks_t *callbacks);

/**
 * @brief Destroy the resolver session and cancel outstanding requests.
 * @param s Session returned by jdns_session_new().
 */
JDNS_EXPORT void jdns_session_delete(jdns_session_t *s);

/**
 * @param s session
 * @param callbacks Datagram bind/read/write transport vtable.
 * @param addr ip address of interface to bind to.  NULL for all
 * @param port port of interface to bind to.  0 for any
 * @return 1 on success, 0 on failure
 */
JDNS_EXPORT int jdns_init_unicast(jdns_session_t *s, const jdns_datagram_callbacks_t *callbacks,
                                  const jdns_address_t *addr, int port);

/**
 * @brief Initialize native asynchronous unicast DNS without binding a socket.
 * @param s Session with shared clock/entropy/debug callbacks.
 * @param callbacks Native async send/cancel transport vtable.
 * @return 1 on success or 0 if required callbacks are missing.
 * @note The transport receives complete DNS wire requests and reports
 * completion through jdns_async_complete() / jdns_async_fail().
 * No bind/read/write or fake transport handle is used.
 */
JDNS_EXPORT int jdns_init_async_unicast(jdns_session_t *s, const jdns_async_callbacks_t *callbacks);

/**
 * @brief Deliver a complete DNS response for an async request.
 * @param s Initialized native asynchronous unicast session.
 * @param request_id ID passed to async_send(), not DNS transaction ID.
 * @param data Unframed DNS wire response.
 * @param size Response size.
 * @return 1 if accepted; 0 if cancelled, malformed or mismatched.
 * @note Only the query with the matching request ID and DNS header ID
 * can consume the reply. Response records are decoded by JDNS.
 */
JDNS_EXPORT int jdns_async_complete(jdns_session_t *s, int request_id,
    const unsigned char *data, int size);

/**
 * @brief Report an asynchronous I/O failure without synthesizing SERVFAIL.
 * @param s Native async session.
 * @param request_id Request ID from async_send().
 * @param status JDNS_STATUS_ERROR or JDNS_STATUS_TIMEOUT.
 * @return 1 if pending query was failed, 0 for stale/unknown request.
 * @note The associated lookup and dependent CNAME parent are finished with
 * an error; no retries or fallback are performed in JDNS.
 */
JDNS_EXPORT int jdns_async_fail(jdns_session_t *s, int request_id, int status);

/**
 * @brief Initialize multicast DNS with real UDP datagram callbacks.
 * @param s Resolver session. Must not have been initialized previously.
 * @param callbacks Required socket bind/read/write/unbind callback table.
 * @param addr Local interface address to bind, or NULL for all.
 * @param port UDP port to bind, normally 5353; zero selects any available.
 * @param maddr Required multicast group address; cannot be NULL.
 * @return 1 on successful initialization, 0 on failure, including missing
 * callbacks, missing group address or repeated initialization.
 * @note The datagram backend must join the multicast group and configure
 * the multicast TTL/hop limit as described by jdns_datagram_callbacks_t.
 */
JDNS_EXPORT int jdns_init_multicast(jdns_session_t *s, const jdns_datagram_callbacks_t *callbacks,
                                    const jdns_address_t *addr, int port, const jdns_address_t *maddr);

/**
 * @param s session
 */
JDNS_EXPORT void jdns_shutdown(jdns_session_t *s);

/**
 * @brief Set upstream resolvers for datagram-based unicast DNS.
 * @param s Initialized DNS session.
 * @param nslist Resolver addresses and ports to use for UDP unicast.
 * @return 1 if the list was accepted or 0 if unavailable, including native
 * async unicast (which owns its resolver selection) or invalid session state.
 * @note For native async query transports, choose the endpoint on the
 * transport itself; this function does not reconfigure it.
 */
JDNS_EXPORT int jdns_set_nameservers(jdns_session_t *s, const jdns_nameserverlist_t *nslist);

/**
 * @param s session
 */
JDNS_EXPORT void jdns_probe(jdns_session_t *s);

/**
 * @param s session
 * @param name the name to look up
 * @param rtype the record type
 * @return id of this operation
 */
JDNS_EXPORT int jdns_query(jdns_session_t *s, const unsigned char *name, int rtype);

/**
 * @param s session
 * @param id the operation id to cancel
 */
JDNS_EXPORT void jdns_cancel_query(jdns_session_t *s, int id);

/**
 * @note Supported record types: A, AAAA, SRV, CNAME, PTR, TXT, and HINFO.
 *       If the published type is not one of these, raw rdata must be set.
 *
 * @param s session
 * @param mode JDNS_PUBLISH shared or unique
 * @param rec the record data
 * @return id of this operation
 */
JDNS_EXPORT int jdns_publish(jdns_session_t *s, int mode, const jdns_rr_t *rec);

/**
 * @note update only works on successfully published records, and no event is generated for a
 *       successful update.
 *
 * @param s session
 * @param id the operation id to update
 * @param rec the record data
 */
JDNS_EXPORT void jdns_update_publish(jdns_session_t *s, int id, const jdns_rr_t *rec);

/**
 * @param s session
 * @param id the operation id to cancel
 */
JDNS_EXPORT void jdns_cancel_publish(jdns_session_t *s, int id);

/**
 * @param s session
 * @return JDNS_STEP flags OR'd together
 */
JDNS_EXPORT int jdns_step(jdns_session_t *s);

/**
 * @param s session
 * @return milliseconds until timeout
 */
JDNS_EXPORT int jdns_next_timer(jdns_session_t *s);

/**
 * @param s session
 * @param handle handle that is now readable
 */
JDNS_EXPORT void jdns_set_handle_readable(jdns_session_t *s, int handle);

/**
 * @param s session
 * @param handle handle that is now writable
 */
JDNS_EXPORT void jdns_set_handle_writable(jdns_session_t *s, int handle);

/**
 * @param s session
 * @return newly allocated event, or zero if none are ready
 */
JDNS_EXPORT jdns_event_t *jdns_next_event(jdns_session_t *s);

/**
 * @return newly allocated dnsparams from the system
 */
JDNS_EXPORT jdns_dnsparams_t *jdns_system_dnsparams();

/**
 * .
 *
 * Normally, when a unicast query completes or any kind of query or publish
 * operation results in an error, the operation is automatically "canceled".
 * When id holding is enabled, the operation still stops internally, but the
 * id value used by that operation is "held" until the application
 * explicitly calls jdns_cancel_query() or jdns_cancel_publish() to release
 * it. This allows the application to ensure there is no ambiguity when
 * determining which operation a particular event belongs to. It is disabled
 * be default so as to not introduce memory leaks in existing applications,
 * however new applications really should use it.
 *
 * @param s session
 * @param enabled whether to enable id holding. Default is 0 (disabled)
 */
JDNS_EXPORT void jdns_set_hold_ids_enabled(jdns_session_t *s, int enabled);

#ifdef __cplusplus
}
#endif

#endif
