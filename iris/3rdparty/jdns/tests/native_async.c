/*
 * Native asynchronous DNS core contract tests, no Qt or network access.
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
    int clock_ms;
    int next_random;
    int sends;
    int cancels;
    int sent_id;
    int cancelled_id;
    int sent_length;
    unsigned char sent[4096];
} Fixture;

static int clock_now(jdns_session_t *session, void *app)
{
    (void)session;
    return ((Fixture *)app)->clock_ms;
}
static int random_number(jdns_session_t *session, void *app)
{
    (void)session;
    return ++((Fixture *)app)->next_random;
}
static void debug_line(jdns_session_t *session, void *app, const char *line)
{
    (void)session;
    (void)app;
    (void)line;
}
static void async_send(jdns_session_t *session, void *app, int id,
                       const unsigned char *data, int size)
{
    Fixture *f = (Fixture *)app;
    (void)session;
    assert(size > 12 && size <= (int)sizeof(f->sent));
    ++f->sends;
    f->sent_id = id;
    f->sent_length = size;
    memcpy(f->sent, data, (size_t)size);
}
static void async_cancel(jdns_session_t *session, void *app, int id)
{
    Fixture *f = (Fixture *)app;
    (void)session;
    ++f->cancels;
    f->cancelled_id = id;
}
static jdns_session_t *new_session(Fixture *f)
{
    jdns_callbacks_t cb;
    jdns_async_callbacks_t async_cb;
    jdns_session_t *s;
    memset(&cb, 0, sizeof(cb));
    memset(&async_cb, 0, sizeof(async_cb));
    cb.app = f;
    cb.time_now = clock_now;
    cb.rand_int = random_number;
    cb.debug_line = debug_line;
    async_cb.send = async_send;
    async_cb.cancel = async_cancel;
    s = jdns_session_new(&cb);
    assert(s && jdns_init_async_unicast(s, &async_cb));
    return s;
}
static void pump(jdns_session_t *s)
{
    int i;
    for(i = 0; i < 3; ++i)
        jdns_step(s);
}
static void put16(unsigned char *out, unsigned short v)
{
    out[0] = (unsigned char)(v >> 8);
    out[1] = (unsigned char)v;
}
static int make_reply(const Fixture *f, unsigned char *out, int capacity,
                      int rcode, int cname)
{
    int len = f->sent_length;
    int target_length = 0;
    const unsigned char target[] = { 5, 'a', 'l', 'i', 'a', 's',
                                     7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
                                     3, 'o', 'r', 'g', 0 };
    if(cname)
        target_length = (int)sizeof(target);
    assert(len + 16 + target_length <= capacity);
    memcpy(out, f->sent, (size_t)len);
    out[2] = 0x81; out[3] = (unsigned char)(0x80 | rcode);
    put16(out + 6, (unsigned short)(rcode == 0 ? 1 : 0));
    if(rcode)
        return len;

    out[len++] = 0xc0;
    out[len++] = 0x0c;
    put16(out + len, (unsigned short)(cname ? JDNS_RTYPE_CNAME : JDNS_RTYPE_A));
    len += 2;
    put16(out + len, 1); len += 2;
    out[len++] = 0; out[len++] = 0; out[len++] = 0; out[len++] = 60;
    put16(out + len, (unsigned short)(cname ? target_length : 4));
    len += 2;
    if(cname) {
        memcpy(out + len, target, (size_t)target_length);
        len += target_length;
    } else {
        out[len++] = 203;
        out[len++] = 0;
        out[len++] = 113;
        out[len++] = 12;
    }
    return len;
}
static int take_event(jdns_session_t *s, int expected_id, int expected_status)
{
    jdns_event_t *e = jdns_next_event(s);
    if(!e)
        return 0;
    assert(e->type == JDNS_EVENT_RESPONSE);
    assert(e->id == expected_id);
    assert(e->status == expected_status);
    if(expected_status == JDNS_STATUS_SUCCESS)
        assert(e->response && e->response->answerCount >= 1);
    jdns_event_delete(e);
    return 1;
}
static void success_and_cache(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int id = jdns_query(s, (const unsigned char *)"cache.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    assert(f.sends == 1 && f.cancels == 0);
    len = make_reply(&f, reply, sizeof(reply), 0, 0);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, id, JDNS_STATUS_SUCCESS));
    jdns_cancel_query(s, id);
    assert(f.cancels == 0);
    id = jdns_query(s, (const unsigned char *)"cache.example.org.", JDNS_RTYPE_A);
    pump(s);
    assert(f.sends == 1); /* TTL cache: no new network operation */
    assert(take_event(s, id, JDNS_STATUS_SUCCESS));
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}
static void duplicate_consumers(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    int id1 = jdns_query(s, (const unsigned char *)"same.example.org.", JDNS_RTYPE_A);
    int id2 = jdns_query(s, (const unsigned char *)"same.example.org.", JDNS_RTYPE_A);
    assert(id1 != id2);
    pump(s);
    assert(f.sends == 1);
    jdns_cancel_query(s, id1);
    assert(f.cancels == 0);
    jdns_cancel_query(s, id2);
    assert(f.cancels == 1 && f.cancelled_id == f.sent_id);
    assert(!jdns_async_fail(s, f.sent_id, JDNS_STATUS_ERROR));
    jdns_session_delete(s);
}
static void cancellation_before_submit(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    int id = jdns_query(s, (const unsigned char *)"never.example.org.", JDNS_RTYPE_A);
    jdns_cancel_query(s, id);
    pump(s);
    assert(f.sends == 0 && f.cancels == 0);
    jdns_session_delete(s);
}
static void timeout_and_error(int status)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    int id = jdns_query(s, (const unsigned char *)"fail.example.org.", JDNS_RTYPE_A);
    pump(s);
    assert(f.sends == 1);
    assert(jdns_async_fail(s, f.sent_id, status));
    assert(take_event(s, id, status));
    assert(!jdns_async_fail(s, f.sent_id, JDNS_STATUS_ERROR));
    assert(f.sends == 1 && f.cancels == 0);
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}
static void invalid_answer(int kind)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int id = jdns_query(s, (const unsigned char *)"bad.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    len = make_reply(&f, reply, sizeof(reply), 0, 0);
    if(kind == 0) reply[1] ^= 0x01; /* wrong transaction ID */
    if(kind == 1) reply[2] &= 0x7f; /* not a DNS response */
    if(kind == 2) len = 9;        /* truncated */
    if(kind == 3) reply[13] ^= 1; /* different QNAME */
    if(kind == 4) reply[f.sent_length - 1] = 3; /* CH class instead of IN */
    if(kind == 5) reply[5] = 2; /* invalid DNS question count */
    assert(!jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, id, JDNS_STATUS_ERROR));
    assert(f.sends == 1 && f.cancels == 0);
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}
static void nxdomain(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int id = jdns_query(s, (const unsigned char *)"missing.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    len = make_reply(&f, reply, sizeof(reply), 3, 0);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, id, JDNS_STATUS_NXDOMAIN));
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}
static void cname_chain(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int id = jdns_query(s, (const unsigned char *)"cname.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    len = make_reply(&f, reply, sizeof(reply), 0, 1);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    pump(s);
    assert(f.sends == 2); /* CNAME target triggers another wire query */
    len = make_reply(&f, reply, sizeof(reply), 0, 0);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, id, JDNS_STATUS_SUCCESS));
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}
static void shutdown_cancels(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    (void)jdns_query(s, (const unsigned char *)"shutdown.example.org.", JDNS_RTYPE_A);
    pump(s);
    assert(f.sends == 1);
    jdns_session_delete(s);
    assert(f.cancels == 1);
}
static void shared_query_delivers_to_all_consumers(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int a = jdns_query(s, (const unsigned char *)"shared.example.org.", JDNS_RTYPE_A);
    int b = jdns_query(s, (const unsigned char *)"shared.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    assert(f.sends == 1 && a != b);
    len = make_reply(&f, reply, sizeof(reply), 0, 0);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, a, JDNS_STATUS_SUCCESS));
    assert(take_event(s, b, JDNS_STATUS_SUCCESS));
    jdns_cancel_query(s, a);
    jdns_cancel_query(s, b);
    jdns_session_delete(s);
}

static void ttl_expiration_reissues_query(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int id = jdns_query(s, (const unsigned char *)"ttl.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    len = make_reply(&f, reply, sizeof(reply), 0, 0);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, id, JDNS_STATUS_SUCCESS));
    jdns_cancel_query(s, id);
    f.clock_ms = 61000; /* record TTL was 60 seconds */
    pump(s);
    id = jdns_query(s, (const unsigned char *)"ttl.example.org.", JDNS_RTYPE_A);
    pump(s);
    assert(f.sends == 2);
    jdns_cancel_query(s, id);
    assert(f.cancels == 1);
    jdns_session_delete(s);
}

static void cname_child_failure_propagates(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    unsigned char reply[4096];
    int id = jdns_query(s, (const unsigned char *)"cnerr.example.org.", JDNS_RTYPE_A);
    int len;
    pump(s);
    len = make_reply(&f, reply, sizeof(reply), 0, 1);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    pump(s);
    assert(f.sends == 2);
    assert(jdns_async_fail(s, f.sent_id, JDNS_STATUS_TIMEOUT));
    assert(take_event(s, id, JDNS_STATUS_TIMEOUT));
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}

static void no_nameserver_configuration_or_duplicate_completion(void)
{
    Fixture f = {0};
    jdns_session_t *s = new_session(&f);
    jdns_nameserverlist_t *nameservers = jdns_nameserverlist_new();
    unsigned char reply[4096];
    int id, len;
    assert(!jdns_set_nameservers(s, nameservers));
    jdns_nameserverlist_delete(nameservers);
    id = jdns_query(s, (const unsigned char *)"once.example.org.", JDNS_RTYPE_A);
    pump(s);
    len = make_reply(&f, reply, sizeof(reply), 0, 0);
    assert(jdns_async_complete(s, f.sent_id, reply, len));
    assert(!jdns_async_complete(s, f.sent_id, reply, len));
    assert(take_event(s, id, JDNS_STATUS_SUCCESS));
    jdns_cancel_query(s, id);
    jdns_session_delete(s);
}

static void incomplete_callbacks_fail_without_network_io(void)
{
    Fixture f = {0};
    jdns_callbacks_t callbacks;
    jdns_async_callbacks_t async_cb;
    jdns_datagram_callbacks_t dg;
    jdns_session_t *s;
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&async_cb, 0, sizeof(async_cb));
    memset(&dg, 0, sizeof(dg));
    assert(!jdns_session_new(NULL));
    assert(!jdns_session_new(&callbacks));
    callbacks.app = &f;
    callbacks.time_now = clock_now;
    callbacks.rand_int = random_number;
    callbacks.debug_line = debug_line;
    s = jdns_session_new(&callbacks);
    assert(s);
    assert(!jdns_init_async_unicast(s, NULL));
    assert(!jdns_init_async_unicast(s, &async_cb));
    assert(!jdns_init_unicast(s, &dg, NULL, 0));
    assert(!jdns_init_multicast(s, &dg, NULL, 5353, NULL));
    async_cb.send = async_send;
    async_cb.cancel = async_cancel;
    assert(jdns_init_async_unicast(s, &async_cb));
    assert(!jdns_init_async_unicast(s, &async_cb));
    {
        jdns_rr_t *rr = jdns_rr_new();
        jdns_rr_set_owner(rr, (const unsigned char *)"record.local.");
        jdns_rr_set_PTR(rr, (const unsigned char *)"target.local.");
        rr->ttl = 120;
        assert(jdns_publish(s, JDNS_PUBLISH_SHARED, rr) == -1);
        jdns_update_publish(s, 0, rr);  // must not access mDNS state
        jdns_cancel_publish(s, 0);
        jdns_rr_delete(rr);
    }
    assert(f.sends == 0 && f.cancels == 0);
    jdns_session_delete(s);
}

int main(void)
{
    incomplete_callbacks_fail_without_network_io();
    success_and_cache();
    shared_query_delivers_to_all_consumers();
    ttl_expiration_reissues_query();
    cname_child_failure_propagates();
    no_nameserver_configuration_or_duplicate_completion();
    duplicate_consumers();
    cancellation_before_submit();
    timeout_and_error(JDNS_STATUS_ERROR);
    timeout_and_error(JDNS_STATUS_TIMEOUT);
    invalid_answer(0);
    invalid_answer(1);
    invalid_answer(2);
    invalid_answer(3);
    invalid_answer(4);
    invalid_answer(5);
    nxdomain();
    cname_chain();
    shutdown_cancels();
    puts("native async-unicast core tests passed");
    return 0;
}
