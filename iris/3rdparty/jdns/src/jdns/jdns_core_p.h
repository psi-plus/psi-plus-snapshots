/*
 * Private resolver/mDNS integration boundary.
 * Copyright (C) 2005-2008 Justin Karneges
 *
 * Internal to libjdns: not installed and not part of the public API.
 * One-shot unicast resolution lives in jdns.c; multicast-specific protocol
 * handling and state transitions live in jdns_multicast.c.
 */
#ifndef JDNS_CORE_P_H
#define JDNS_CORE_P_H

#include "jdns_p.h"
#include "jdns_mdnsd.h"

typedef struct list_item
{
    void (*dtor)(void *);
} list_item_t;

typedef struct list
{
    int count;
    list_item_t **item;
} list_t;

typedef struct query
{
    void (*dtor)(void *);

    int id;

    // user request ids
    int req_ids_count;
    int *req_ids;

    // packet id
    int dns_id;
    int async_inflight; /* native async transport currently owns this query */

    // what we are looking up
    unsigned char *qname;
    int qtype;

    // how many transmission attempts we have done.  note this
    //  is not actually how many packets have been sent, since
    //  it is possible for the first transmission to send many
    //  at once.  this variable lets us decide when to give up.
    //  (idea taken from qdns).
    // set to -1 to deactivate (stop sending packets)
    int step;

    // which nameservers we've tried (stored as a list of ids)
    int servers_tried_count;
    int *servers_tried;

    // which servers we shouldn't try again
    int servers_failed_count;
    int *servers_failed;

    // flag to indicate whether or not we've tried all available
    //  nameservers already.  this means that all future
    //  transmissions are likely repeats, and should be slowed
    //  down.
    int retrying;

    // flag to indicate if we've received nxdomain as an error so far
    int nxdomain;

    // holds a timeout for the next step (time_start == -1 means no timer)
    int time_start;
    int time_next;

    // whether or not to look in the cache for this query
    int trycache;

    // cname subquerying.  only cname_parent or cname_child may be set,
    //  never both.
    int cname_chain_count;
    struct query *cname_parent;
    struct query *cname_child;

    // accumulates known multicast records to prevent duplicates
    jdns_response_t *mul_known;
} query_t;

typedef struct event
{
    void (*dtor)(void *);
    jdns_event_t *event;
} event_t;

typedef struct published_item
{
    void (*dtor)(void *);
    int id;
    int mode;
    unsigned char *qname;
    int qtype;
    mdnsdr rec;
    jdns_rr_t *rr;
} published_item_t;

struct jdns_session
{
    jdns_callbacks_t cb;
    jdns_datagram_callbacks_t datagram;
    jdns_async_callbacks_t async;
    int mode;
    int initialized; /* exactly one of async unicast, datagram unicast, mDNS */
    int async_unicast;
    int shutdown;
    int next_qid;
    int next_req_id;
    int last_time;
    int next_timer;
    int next_name_server_id;
    int handle;
    int handle_readable, handle_writable;
    int port;
    list_t *name_servers;
    list_t *queries;
    list_t *outgoing;
    list_t *events;
    list_t *cache;

    // for blocking req_ids from reuse until user explicitly releases
    int do_hold_req_ids;
    int held_req_ids_count;
    int *held_req_ids;

    // mdns
    mdnsd mdns;
    list_t *published;
    jdns_address_t *maddr;
};

/* Shared resolver helpers consumed by the multicast module. */
jdns_event_t *jdns_event_new(void);
int _cmp_rr(const jdns_rr_t *a, const jdns_rr_t *b);
void _print_hexdump(jdns_session_t *s, const unsigned char *buf, int size);
void _print_packet(jdns_session_t *s, const jdns_packet_t *packet);
void _print_records(jdns_session_t *s, const jdns_response_t *r, const unsigned char *owner);
jdns_string_t *_make_printable_cstr(const char *str);
const char *_qtype2str(int qtype);
unsigned char *_fix_input(const unsigned char *in);
jdns_response_t *_packet2response(const jdns_packet_t *packet, const unsigned char *qname, int qtype, int classmask);
void _debug_line(jdns_session_t *s, const char *format, ...);
void _append_event(jdns_session_t *s, jdns_event_t *event);
void _append_event_and_hold_id(jdns_session_t *s, jdns_event_t *event);
int get_next_qid(jdns_session_t *s);
int get_next_req_id(jdns_session_t *s);
void list_insert(list_t *l, void *item, int pos);
void list_remove(list_t *l, void *item);
query_t *query_new();
void query_add_req_id(query_t *q, int req_id);
int query_have_req_id(const query_t *q, int req_id);
void query_remove_req_id(query_t *q, int req_id);
published_item_t *published_item_new();

/* mDNS entry points, isolated from the unicast resolver implementation. */
int _callback_time_now(mdnsd d, void *arg);
int _callback_rand_int(mdnsd d, void *arg);
int _multicast_query(jdns_session_t *s, const unsigned char *name, int qtype);
void _multicast_cancel(jdns_session_t *s, int req_id);
int _multicast_publish(jdns_session_t *s, int mode, const jdns_rr_t *rr);
void _multicast_update_publish(jdns_session_t *s, int id, const jdns_rr_t *rr);
void _multicast_cancel_publish(jdns_session_t *s, int id);
void _multicast_flush(jdns_session_t *s);
int jdns_step_multicast(jdns_session_t *s, int now);

#endif /* JDNS_CORE_P_H */
