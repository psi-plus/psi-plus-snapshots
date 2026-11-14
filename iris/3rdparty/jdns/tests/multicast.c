/*
 * Multicast DNS isolation/regression tests. No sockets or Qt are needed.
 * SPDX-License-Identifier: MIT
 */
#include "jdns.h"
#ifdef NDEBUG
#undef NDEBUG // Test invariants must remain active in CMake Release builds.
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int now;
    int seed;
    int binds;
    int unbinds;
    int sends;
    int reads;
    int fail_writes;
    int last_destination_port;
    int bind_multicast;
    int received_length;
    unsigned char received[1024];
    unsigned char sent[9000];
    int sent_length;
} FakeNetwork;

static int clock_now(jdns_session_t *s, void *app)
{
    (void)s;
    return ((FakeNetwork *)app)->now;
}
static int random_number(jdns_session_t *s, void *app)
{
    (void)s;
    return ++((FakeNetwork *)app)->seed;
}
static void debug_log(jdns_session_t *s, void *app, const char *line)
{
    (void)s; (void)app; (void)line;
}
static int bind_udp(jdns_session_t *s, void *app, const jdns_address_t *address,
                    int port, const jdns_address_t *multicast)
{
    FakeNetwork *f = (FakeNetwork *)app;
    (void)s; (void)address;
    assert(port == 5353);
    ++f->binds;
    if(multicast)
        ++f->bind_multicast;
    return 7;
}
static void unbind_udp(jdns_session_t *s, void *app, int handle)
{
    (void)s;
    assert(handle == 7);
    ++((FakeNetwork *)app)->unbinds;
}
static int receive_udp(jdns_session_t *s, void *app, int handle,
                       jdns_address_t *address, int *port,
                       unsigned char *buf, int *size)
{
    FakeNetwork *f = (FakeNetwork *)app;
    (void)s;
    assert(handle == 7);
    ++f->reads;
    if(!f->received_length)
        return 0;
    assert(*size >= f->received_length);
    memcpy(buf, f->received, (size_t)f->received_length);
    *size = f->received_length;
    f->received_length = 0;
    jdns_address_set_ipv4(address, 0x7f000001);
    *port = 5353;
    return 1;
}
static int transmit_udp(jdns_session_t *s, void *app, int handle,
                        const jdns_address_t *address, int port,
                        unsigned char *packet, int size)
{
    FakeNetwork *f = (FakeNetwork *)app;
    (void)s;
    assert(handle == 7 && address != NULL);
    assert(size >= 12 && size <= (int)sizeof(f->sent));
    if(f->fail_writes) {
        --f->fail_writes;
        return 0;
    }
    memcpy(f->sent, packet, (size_t)size);
    f->sent_length = size;
    f->last_destination_port = port;
    ++f->sends;
    return 1;
}
static jdns_session_t *new_multicast(FakeNetwork *f)
{
    jdns_callbacks_t common = {0};
    jdns_datagram_callbacks_t io = {0};
    jdns_session_t *s;
    jdns_address_t *address, *group;
    common.app = f;
    common.time_now = clock_now;
    common.rand_int = random_number;
    common.debug_line = debug_log;
    s = jdns_session_new(&common);
    assert(s);
    io.transport_bind = bind_udp;
    io.transport_unbind = unbind_udp;
    io.transport_read = receive_udp;
    io.transport_write = transmit_udp;
    address = jdns_address_new();
    jdns_address_set_ipv4(address, 0);
    group = jdns_address_multicast4_new();
    assert(jdns_init_multicast(s, &io, address, 5353, group));
    jdns_address_delete(group);
    jdns_address_delete(address);
    assert(f->binds == 1 && f->bind_multicast == 1);
    return s;
}
static void pump(jdns_session_t *s, FakeNetwork *f, int count)
{
    int n;
    for(n = 0; n < count; ++n) {
        f->now += 300;
        (void)jdns_step(s);
    }
}
static jdns_rr_t *new_ptr_record(void)
{
    jdns_rr_t *r = jdns_rr_new();
    jdns_rr_set_owner(r, (const unsigned char *)"_demo._tcp.local.");
    jdns_rr_set_PTR(r, (const unsigned char *)"sample._demo._tcp.local.");
    r->ttl = 120;
    assert(jdns_rr_verify(r));
    return r;
}
static jdns_event_t *next_event(jdns_session_t *s)
{
    return jdns_next_event(s);
}
static void publication_lifecycle(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    jdns_rr_t *rr = new_ptr_record();
    jdns_event_t *event;
    int first = jdns_publish(s, JDNS_PUBLISH_SHARED, rr);
    assert(first >= 0);
    event = next_event(s);
    assert(event && event->type == JDNS_EVENT_PUBLISH &&
           event->id == first && event->status == JDNS_STATUS_SUCCESS);
    jdns_event_delete(event);

    /* A duplicate type+name must be rejected and report a typed error. */
    {
        int duplicate = jdns_publish(s, JDNS_PUBLISH_SHARED, rr);
        event = next_event(s);
        assert(event && event->type == JDNS_EVENT_PUBLISH &&
               event->id == duplicate && event->status == JDNS_STATUS_ERROR);
        jdns_event_delete(event);
    }
    pump(s, &f, 20);
    assert(f.sends > 0);
    assert(f.last_destination_port == 5353);
    assert(f.sent_length >= 12);
    jdns_update_publish(s, first, rr);
    jdns_cancel_publish(s, first);
    jdns_rr_delete(rr);
    jdns_session_delete(s);
    assert(f.unbinds == 1);
}
static void reject_wrong_lifecycle(void)
{
    FakeNetwork f = {0};
    jdns_callbacks_t common = {0};
    jdns_session_t *uninitialized;
    jdns_session_t *multicast;
    jdns_datagram_callbacks_t dg = {0};
    jdns_address_t *maddr;
    jdns_rr_t *rr;
    common.app = &f;
    common.time_now = clock_now;
    common.rand_int = random_number;
    common.debug_line = debug_log;
    uninitialized = jdns_session_new(&common);
    assert(uninitialized);
    assert(jdns_query(uninitialized, (const unsigned char *)"example.local.", JDNS_RTYPE_PTR) == -1);
    assert(jdns_step(uninitialized) == 0);
    dg.transport_bind = bind_udp;
    dg.transport_unbind = unbind_udp;
    dg.transport_read = receive_udp;
    dg.transport_write = transmit_udp;
    assert(!jdns_init_multicast(uninitialized, &dg, NULL, 5353, NULL));
    assert(f.binds == 0); // invalid multicast group must not open a socket
    jdns_session_delete(uninitialized);

    multicast = new_multicast(&f);
    maddr = jdns_address_multicast4_new();
    assert(!jdns_init_multicast(multicast, &dg, NULL, 5353, maddr));
    assert(f.binds == 1); // double init must not rebind
    jdns_address_delete(maddr);
    rr = new_ptr_record();
    assert(jdns_publish(multicast, JDNS_PUBLISH_SHARED, rr) >= 0);
    jdns_rr_delete(rr);
    jdns_session_delete(multicast);
    assert(f.unbinds == 1);
}

static void query_reuse_cancel_and_probe(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    int a = jdns_query(s, (const unsigned char *)"_demo._tcp.local.", JDNS_RTYPE_PTR);
    int b = jdns_query(s, (const unsigned char *)"_demo._tcp.local.", JDNS_RTYPE_PTR);
    assert(a != b);
    pump(s, &f, 20);
    assert(f.sends >= 1);
    jdns_cancel_query(s, a);
    /* The second observer is still subscribed. */
    jdns_probe(s);
    pump(s, &f, 20);
    jdns_cancel_query(s, b);
    jdns_session_delete(s);
    assert(f.unbinds == 1);
}
static void malformed_multicast_and_shutdown(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    jdns_event_t *event = NULL;
    /* Malformed datagram must be ignored, not delivered as an answer. */
    f.received[0] = 0x42;
    f.received_length = 1;
    jdns_set_handle_readable(s, 7);
    pump(s, &f, 1);
    assert(f.reads >= 1);
    assert(next_event(s) == NULL);
    jdns_shutdown(s);
    pump(s, &f, 20);
    while((event = next_event(s)) != NULL) {
        int shutdown = event->type == JDNS_EVENT_SHUTDOWN;
        jdns_event_delete(event);
        if(shutdown)
            break;
    }
    assert(event != NULL);
    jdns_session_delete(s);
    assert(f.unbinds == 1);
}
static void put16(unsigned char *dst, unsigned int number)
{
    dst[0] = (unsigned char)(number >> 8);
    dst[1] = (unsigned char)number;
}
static int append_dns_name(unsigned char *packet, int offset, const char *domain)
{
    const char *p = domain;
    while(*p) {
        const char *dot = strchr(p, '.');
        size_t length = dot ? (size_t)(dot - p) : strlen(p);
        assert(length > 0 && length < 64);
        assert(offset + (int)length + 1 < 1024);
        packet[offset++] = (unsigned char)length;
        memcpy(packet + offset, p, length);
        offset += (int)length;
        if(!dot)
            break;
        p = dot + 1;
    }
    packet[offset++] = 0;
    return offset;
}
static int make_ptr_response(unsigned char *packet, int ttl)
{
    int at, rdata_at;
    memset(packet, 0, 1024);
    packet[2] = 0x84; // DNS QR + AA, per RFC 6762
    packet[7] = 1;    // one answer, zero questions
    at = 12;
    at = append_dns_name(packet, at, "_demo._tcp.local.");
    put16(packet + at, JDNS_RTYPE_PTR); at += 2;
    put16(packet + at, 1); at += 2;
    packet[at++] = 0; packet[at++] = 0;
    put16(packet + at, (unsigned int)ttl); at += 2;
    rdata_at = at;
    at += 2;
    {
        int start = at;
        at = append_dns_name(packet, at, "sample._demo._tcp.local.");
        put16(packet + rdata_at, (unsigned int)(at - start));
    }
    return at;
}
static void received_ptr_and_goodbye(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    int q = jdns_query(s, (const unsigned char *)"_demo._tcp.local.", JDNS_RTYPE_PTR);
    jdns_event_t *e;
    assert(q >= 0);
    pump(s, &f, 3);
    f.received_length = make_ptr_response(f.received, 120);
    jdns_set_handle_readable(s, 7);
    pump(s, &f, 1);
    e = next_event(s);
    assert(e && e->type == JDNS_EVENT_RESPONSE && e->id == q &&
           e->status == JDNS_STATUS_SUCCESS);
    assert(e->response && e->response->answerCount == 1);
    assert(e->response->answerRecords[0]->type == JDNS_RTYPE_PTR);
    assert(e->response->answerRecords[0]->ttl == 120);
    jdns_event_delete(e);

    /* A second subscriber receives the current cached record. */
    {
        int next = jdns_query(s, (const unsigned char *)"_demo._tcp.local.", JDNS_RTYPE_PTR);
        assert(next != q);
        e = next_event(s);
        assert(e && e->id == next && e->status == JDNS_STATUS_SUCCESS);
        jdns_event_delete(e);
        jdns_cancel_query(s, next);
    }
    f.received_length = make_ptr_response(f.received, 0); // goodbye
    jdns_set_handle_readable(s, 7);
    pump(s, &f, 1);
    e = next_event(s);
    assert(e && e->type == JDNS_EVENT_RESPONSE && e->id == q);
    /* RFC 6762 goodbye = record deletion; keep it distinct from RFC 8765
     * RRset deletion (TTL 0xFFFFFFFE). */
    assert(e->response && e->response->answerCount == 1);
    assert(e->response->answerRecords[0]->ttl == 0);
    jdns_event_delete(e);
    jdns_cancel_query(s, q);
    jdns_session_delete(s);
    assert(f.unbinds == 1);
}

static void temporary_output_backpressure(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    jdns_rr_t *rr = new_ptr_record();
    jdns_event_t *event;
    int id = jdns_publish(s, JDNS_PUBLISH_SHARED, rr);
    event = next_event(s);
    assert(event && event->status == JDNS_STATUS_SUCCESS);
    jdns_event_delete(event);
    f.fail_writes = 1;
    pump(s, &f, 12);
    /* A writable notification must allow queued sends to resume. */
    jdns_set_handle_writable(s, 7);
    pump(s, &f, 20);
    assert(f.sends > 0);
    jdns_cancel_publish(s, id);
    jdns_rr_delete(rr);
    jdns_session_delete(s);
}

static void unique_update_ignores_old_goodbye(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    jdns_rr_t *rr = new_ptr_record();
    int id = jdns_publish(s, JDNS_PUBLISH_UNIQUE, rr);
    jdns_event_t *event;
    pump(s, &f, 20);
    event = next_event(s);
    assert(event && event->type == JDNS_EVENT_PUBLISH && event->id == id
           && event->status == JDNS_STATUS_SUCCESS);
    jdns_event_delete(event);

    jdns_rr_set_PTR(rr, (const unsigned char *)"updated._demo._tcp.local.");
    jdns_update_publish(s, id, rr);
    /* An old cached goodbye is not competing ownership of the new value. */
    f.received_length = make_ptr_response(f.received, 0);
    jdns_set_handle_readable(s, 7);
    pump(s, &f, 20);
    while((event = next_event(s)) != NULL) {
        assert(event->status == JDNS_STATUS_SUCCESS);
        jdns_event_delete(event);
    }
    jdns_cancel_publish(s, id);
    jdns_rr_delete(rr);
    jdns_session_delete(s);
}

static void cancelling_non_head_publication_unlinks_all_queues(void)
{
    FakeNetwork f = {0};
    jdns_session_t *s = new_multicast(&f);
    jdns_rr_t *first = new_ptr_record();
    jdns_rr_t *second = jdns_rr_copy(first);
    jdns_event_t *event;
    int a, b;
    jdns_rr_set_PTR(second, (const unsigned char *)"second._demo._tcp.local.");
    a = jdns_publish(s, JDNS_PUBLISH_SHARED, first);
    b = jdns_publish(s, JDNS_PUBLISH_SHARED, second);
    assert(a != b);
    while((event = next_event(s)) != NULL) {
        assert(event->status == JDNS_STATUS_SUCCESS);
        jdns_event_delete(event);
    }
    // Both are still queued for announcements, with 'first' not at the head.
    jdns_cancel_publish(s, a);
    pump(s, &f, 5);
    assert(f.sends > 0);
    jdns_cancel_publish(s, b);
    pump(s, &f, 5);
    jdns_shutdown(s);
    pump(s, &f, 5);
    jdns_rr_delete(first);
    jdns_rr_delete(second);
    jdns_session_delete(s);
}
int main(void)
{
    reject_wrong_lifecycle();
    publication_lifecycle();
    received_ptr_and_goodbye();
    query_reuse_cancel_and_probe();
    malformed_multicast_and_shutdown();
    temporary_output_backpressure();
    unique_update_ignores_old_goodbye();
    cancelling_non_head_publication_unlinks_all_queues();
    puts("mDNS isolation regression tests passed");
    return 0;
}
