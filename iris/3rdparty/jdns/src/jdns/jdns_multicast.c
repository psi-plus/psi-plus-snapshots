/*
 * Multicast DNS integration and publication logic, extracted from jdns.c.
 * Copyright (C) 2005-2008 Justin Karneges
 * Copyright (C) 2020 Ivan Romanov
 *
 * SPDX-License-Identifier: MIT
 */
#include "jdns_core_p.h"

#define JDNS_UDP_MUL_OUT_MAX 9000
#define JDNS_UDP_MUL_IN_MAX 16384

/* mdnsd's monotonic clock is deliberately offset from zero. */
int _callback_time_now(mdnsd d, void *arg)
{
    jdns_session_t *s = (jdns_session_t *)arg;
    (void)d;
    return s->cb.time_now(s, s->cb.app) + 120 * 1000;
}

int _callback_rand_int(mdnsd d, void *arg)
{
    jdns_session_t *s = (jdns_session_t *)arg;
    (void)d;
    return s->cb.rand_int(s, s->cb.app);
}

//----------------------------------------------------------------------------
// jdns - multicast
//----------------------------------------------------------------------------
static jdns_rr_t *_mdnsda2rr(mdnsda a)
{
    jdns_rr_t *rr;

    if(a->type == JDNS_RTYPE_ANY)
        return 0;

    // for AAAA, TXT and HINFO, run the raw rdata through jdns_rr's parser
    if(a->type == JDNS_RTYPE_AAAA || a->type == JDNS_RTYPE_TXT || a->type == JDNS_RTYPE_HINFO)
    {
        jdns_packet_resource_t *pr = jdns_packet_resource_new();
        pr->qname = jdns_string_new();
        jdns_string_set_cstr(pr->qname, (const char *)a->name);
        pr->qtype = a->type;
        pr->qclass = 0x0001; // class is always 1 for us
        if(a->ttl == 0)
            pr->ttl = 0;
        else
            pr->ttl = a->real_ttl;
        pr->rdata = jdns_copy_array(a->rdata, a->rdlen);
        pr->rdlength = a->rdlen;

        // we don't need a reference for these types
        rr = jdns_rr_from_resource(pr, 0);
        jdns_packet_resource_delete(pr);
    }
    // else, pull the values out of 'a' directly
    else
    {
        rr = jdns_rr_new();
        rr->owner = _ustrdup(a->name);
        rr->qclass = 0x0001; // class is always 1 for us
        if(a->ttl == 0)
            rr->ttl = 0;
        else
            rr->ttl = a->real_ttl;

        switch(a->type)
        {
            case JDNS_RTYPE_A:
            {
                jdns_address_t *addr = jdns_address_new();
                jdns_address_set_ipv4(addr, a->ip);
                jdns_rr_set_A(rr, addr);
                jdns_address_delete(addr);
                break;
            }
            case JDNS_RTYPE_AAAA:
            {
                // covered earlier
                break;
            }
            case JDNS_RTYPE_MX:
            {
                // don't care about MX
                jdns_rr_delete(rr);
                rr = 0;
                break;
            }
            case JDNS_RTYPE_SRV:
            {
                jdns_rr_set_SRV(rr, a->rdname, a->srv.port, a->srv.priority, a->srv.weight);
                break;
            }
            case JDNS_RTYPE_CNAME:
            {
                jdns_rr_set_CNAME(rr, a->rdname);
                break;
            }
            case JDNS_RTYPE_PTR:
            {
                jdns_rr_set_PTR(rr, a->rdname);
                break;
            }
            case JDNS_RTYPE_TXT:
            {
                // covered earlier
                break;
            }
            case JDNS_RTYPE_HINFO:
            {
                // covered earlier
                break;
            }
            case JDNS_RTYPE_NS:
            {
                // don't care about NS
                jdns_rr_delete(rr);
                rr = 0;
                break;
            }
            default:
            {
                jdns_rr_set_record(rr, a->type, a->rdata, a->rdlen);
                break;
            }
        }
    }

    return rr;
}

int _multicast_query_ans(mdnsda a, void *arg)
{
    int n;
    jdns_session_t *s;
    query_t *q;
    jdns_response_t *r;
    jdns_rr_t *rr;

    s = (jdns_session_t *)arg;

    // what query is this for?
    q = 0;
    for(n = 0; n < s->queries->count; ++n)
    {
        query_t *i = (query_t *)s->queries->item[n];
        if((i->qtype == JDNS_RTYPE_ANY || i->qtype == a->type) && jdns_domain_cmp(i->qname, a->name))
        {
            q = i;
            break;
        }
    }

    // note: this can't happen, but we'll check anyway
    if(!q)
    {
        _debug_line(s, "no such multicast query");
        return 0;
    }

    rr = _mdnsda2rr(a);
    if(!rr)
        return 0;

    // add/remove as a known
    if(rr->ttl == 0)
    {
        for(n = 0; n < q->mul_known->answerCount; ++n)
        {
            jdns_rr_t *k = q->mul_known->answerRecords[n];
            if(_cmp_rr(k, rr))
            {
                jdns_response_remove_answer(q->mul_known, n);
                break;
            }
        }
    }
    else
        jdns_response_append_answer(q->mul_known, rr);

    r = jdns_response_new();
    jdns_response_append_answer(r, rr);
    jdns_rr_delete(rr);

    // report event to any requests listening
    for(n = 0; n < q->req_ids_count; ++n)
    {
        jdns_event_t *event;
        event = jdns_event_new();
        event->type = JDNS_EVENT_RESPONSE;
        event->id = q->req_ids[n];
        event->status = JDNS_STATUS_SUCCESS;
        event->response = jdns_response_copy(r);
        _append_event(s, event);
    }

    jdns_response_delete(r);
    return 0;
}

query_t *_get_multicast_query(jdns_session_t *s, const unsigned char *qname, int qtype)
{
    int n;
    query_t *q;
    jdns_string_t *str;

    // check for existing queries
    for(n = 0; n < s->queries->count; ++n)
    {
        q = (query_t *)s->queries->item[n];
        if(jdns_domain_cmp(q->qname, qname) && q->qtype == qtype)
        {
            str = _make_printable_cstr((const char *)q->qname);
            _debug_line(s, "[%d] reusing query for: [%s] [%s]", q->id, _qtype2str(qtype), str->data);
            jdns_string_delete(str);
            return q;
        }
    }

    q = query_new();
    q->id = get_next_qid(s);
    q->qname = _ustrdup(qname);
    q->qtype = qtype;
    q->step = 0;
    q->mul_known = jdns_response_new();
    list_insert(s->queries, q, -1);

    str = _make_printable_cstr((const char *)q->qname);
    _debug_line(s, "[%d] querying: [%s] [%s]", q->id, _qtype2str(qtype), str->data);
    jdns_string_delete(str);
    return q;
}

int _multicast_query(jdns_session_t *s, const unsigned char *name, int qtype)
{
    unsigned char *qname;
    query_t *q;
    int req_id;
    jdns_string_t *str;

    str = _make_printable_cstr((const char *)name);
    _debug_line(s, "query input: [%s]", str->data);
    jdns_string_delete(str);

    // add a dot to the end if needed
    qname = _fix_input(name);

    q = _get_multicast_query(s, qname, qtype);
    req_id = get_next_req_id(s);
    query_add_req_id(q, req_id);
    free(qname);

    // start the mdnsd_query if necessary
    if(q->step == 0)
    {
        q->step = 1;
        mdnsd_query(s->mdns, (char *)q->qname, q->qtype, _multicast_query_ans, s);
    }
    else
    {
        int n;

        // report the knowns
        for(n = 0; n < q->mul_known->answerCount; ++n)
        {
            const jdns_rr_t *rr;
            jdns_response_t *r;
            jdns_event_t *event;

            rr = q->mul_known->answerRecords[n];
            r = jdns_response_new();
            jdns_response_append_answer(r, rr);

            event = jdns_event_new();
            event->type = JDNS_EVENT_RESPONSE;
            event->id = req_id;
            event->status = JDNS_STATUS_SUCCESS;
            event->response = r;
            _append_event(s, event);
        }
    }
    return req_id;
}

void _multicast_cancel(jdns_session_t *s, int req_id)
{
    int n;
    for(n = 0; n < s->queries->count; ++n)
    {
        query_t *q = (query_t *)s->queries->item[n];
        if(query_have_req_id(q, req_id))
        {
            query_remove_req_id(q, req_id);

            // if no one else is depending on this request, then take action
            if(q->req_ids_count == 0)
            {
                mdnsd_query(s->mdns, (char *)q->qname, q->qtype, NULL, 0);
                list_remove(s->queries, q);
            }
            break;
        }
    }
}

void _multicast_pubresult(int result, char *name, int type, void *arg)
{
    jdns_session_t *s;
    published_item_t *pub;
    jdns_event_t *event;
    int n;

    s = (jdns_session_t *)arg;

    // find the associated pub item
    pub = 0;
    for(n = 0; n < s->published->count; ++n)
    {
        published_item_t *i = (published_item_t *)s->published->item[n];
        if(strcmp((char *)i->qname, name) == 0 && i->qtype == type)
        {
            pub = i;
            break;
        }
    }

    // note: this can't happen, but we'll check anyway
    if(!pub)
    {
        _debug_line(s, "no such multicast published item");
        return;
    }

    if(result == 1)
    {
        jdns_string_t *str = _make_printable_cstr(name);
        _debug_line(s, "published name %s for type %d", str->data, type);
        jdns_string_delete(str);

        event = jdns_event_new();
        event->type = JDNS_EVENT_PUBLISH;
        event->id = pub->id;
        event->status = JDNS_STATUS_SUCCESS;
        _append_event(s, event);
    }
    else
    {
        jdns_string_t *str = _make_printable_cstr(name);
        _debug_line(s, "conflicting name detected %s for type %d", str->data, type);
        jdns_string_delete(str);

        event = jdns_event_new();
        event->type = JDNS_EVENT_PUBLISH;
        event->id = pub->id;
        event->status = JDNS_STATUS_CONFLICT;
        _append_event_and_hold_id(s, event);

        // remove the item
        list_remove(s->published, pub);
    }
}

static jdns_string_t *_create_text(const jdns_stringlist_t *texts)
{
    jdns_string_t *out;
    int n;
    int total;
    unsigned char *buf;

    buf = 0;
    total = 0;
    for(n = 0; n < texts->count; ++n)
        total += texts->item[n]->size + 1;
    if(total > 0)
    {
        int at = 0;
        buf = (unsigned char *)malloc(total);
        for(n = 0; n < texts->count; ++n)
        {
            unsigned int len = texts->item[n]->size;
            buf[at++] = len;
            memcpy(buf + at, texts->item[n]->data, len);
            at += len;
        }
    }

    out = jdns_string_new();
    if(buf)
    {
        out->data = buf;
        out->size = total;
    }
    else
    {
        // RFC 6763 section 6: an empty TXT record is one empty character
        // string, not zero octets of RDATA.
        const unsigned char empty = 0;
        jdns_string_set(out, &empty, 1);
    }
    return out;
}

static void _publish_applyrr_unknown(jdns_session_t *s, mdnsdr r, const jdns_rr_t *rr)
{
    // for unsupported/unknown, just take the rdata
    // note: for this to work, the app must explicitly set the rdata.
    //   if the record is MX or some other known but unsupported record
    //   type, setting the known fields is not enough
    mdnsd_set_raw(s->mdns, r, (char *)rr->rdata, rr->rdlength);
}

static int _publish_applyrr(jdns_session_t *s, mdnsdr r, const jdns_rr_t *rr)
{
    if(!rr->haveKnown)
    {
        _publish_applyrr_unknown(s, r, rr);
        return 1;
    }

    // jdns_mdnsd supports: A, AAAA, SRV, CNAME, PTR, TXT, and HINFO
    switch(rr->type)
    {
        case JDNS_RTYPE_A:
        {
            unsigned long int ip_net = htonl(rr->data.address->addr.v4);
            mdnsd_set_raw(s->mdns, r, (char *)&ip_net, 4);
            break;
        }
        case JDNS_RTYPE_AAAA:
        {
            mdnsd_set_raw(s->mdns, r, (char *)rr->data.address->addr.v6, 16);
            break;
        }
        case JDNS_RTYPE_SRV:
        {
            mdnsd_set_srv(s->mdns, r, rr->data.server->priority, rr->data.server->weight, rr->data.server->port, (char *)rr->data.server->name);
            break;
        }
        case JDNS_RTYPE_CNAME:
        {
            mdnsd_set_host(s->mdns, r, (char *)rr->data.name);
            break;
        }
        case JDNS_RTYPE_PTR:
        {
            mdnsd_set_host(s->mdns, r, (char *)rr->data.name);
            break;
        }
        case JDNS_RTYPE_TXT:
        {
            jdns_string_t *out = _create_text(rr->data.texts);
            mdnsd_set_raw(s->mdns, r, (char *)out->data, out->size);
            jdns_string_delete(out);
            break;
        }
        case JDNS_RTYPE_HINFO:
        {
            jdns_string_t *out;
            jdns_stringlist_t *list;

            list = jdns_stringlist_new();
            jdns_stringlist_append(list, rr->data.hinfo.cpu);
            jdns_stringlist_append(list, rr->data.hinfo.os);
            out = _create_text(list);
            jdns_stringlist_delete(list);

            mdnsd_set_raw(s->mdns, r, (char *)out->data, out->size);
            jdns_string_delete(out);
            break;
        }
        default:
        {
            _publish_applyrr_unknown(s, r, rr);
            break;
        }
    }

    return 1;
}

static void report_published(jdns_session_t *s, published_item_t *pub)
{
    jdns_event_t *event;
    jdns_string_t *str;

    str = _make_printable_cstr((char *)pub->qname);
    _debug_line(s, "published name %s for type %d", str->data, pub->qtype);
    jdns_string_delete(str);

    event = jdns_event_new();
    event->type = JDNS_EVENT_PUBLISH;
    event->id = pub->id;
    event->status = JDNS_STATUS_SUCCESS;
    _append_event(s, event);
}

int _multicast_publish(jdns_session_t *s, int mode, const jdns_rr_t *rr)
{
    mdnsdr r;
    published_item_t *pub;
    int next_id;
    jdns_event_t *event;
    int n;

    r = 0;
    next_id = get_next_req_id(s);

    // see if we have an item with this name+type combination already
    pub = 0;
    for(n = 0; n < s->published->count; ++n)
    {
        published_item_t *i = (published_item_t *)s->published->item[n];
        if(i->qtype == rr->type && jdns_domain_cmp(i->qname, rr->owner))
        {
            // DNS-SD browses one shared PTR RRset containing several service
            // instances. Distinct targets need independent publication IDs.
            if(mode == JDNS_PUBLISH_SHARED && i->mode == JDNS_PUBLISH_SHARED
               && rr->type == JDNS_RTYPE_PTR && rr->haveKnown && i->rr->haveKnown
               && !jdns_domain_cmp(i->rr->data.name, rr->data.name))
                continue;
            pub = i;
            break;
        }
    }
    if(pub)
        goto error;

    if(!jdns_rr_verify(rr))
        goto error;

    if(mode == JDNS_PUBLISH_UNIQUE)
        r = mdnsd_unique(s->mdns, (char *)rr->owner, rr->type, rr->ttl, _multicast_pubresult, s);
    else
        r = mdnsd_shared(s->mdns, (char *)rr->owner, rr->type, rr->ttl);

    if(!_publish_applyrr(s, r, rr))
        goto error;

    pub = published_item_new();
    pub->id = next_id;
    pub->mode = mode;
    pub->qname = _ustrdup(rr->owner);
    pub->qtype = rr->type;
    pub->rec = r;
    pub->rr = jdns_rr_copy(rr);
    list_insert(s->published, pub, -1);

    // mdnsd doesn't report publish events for shared, so do that here
    if(mode == JDNS_PUBLISH_SHARED)
        report_published(s, pub);

    return pub->id;

error:
    _debug_line(s, "attempt to publish record, malformed, unsupported, or duplicate type");

    if(r)
    {
        // don't publish
        mdnsd_done(s->mdns, r);
    }

    // send an error to the app
    event = jdns_event_new();
    event->type = JDNS_EVENT_PUBLISH;
    event->id = next_id;
    event->status = JDNS_STATUS_ERROR;
    _append_event_and_hold_id(s, event);

    return next_id;
}

void _multicast_update_publish(jdns_session_t *s, int id, const jdns_rr_t *rr)
{
    mdnsdr r;
    published_item_t *pub;
    int n;

    pub = 0;
    for(n = 0; n < s->published->count; ++n)
    {
        published_item_t *i = (published_item_t *)s->published->item[n];
        if(i->id == id)
        {
            pub = i;
            break;
        }
    }
    if(!pub)
        return;

    // An update cannot change the identity of the published RRset.
    if(rr->type != pub->qtype || !jdns_domain_cmp(rr->owner, pub->qname) || !jdns_rr_verify(rr))
        return;

    r = pub->rec;

    // We still own a unique name: update it in place using cache-flush.
    // Releasing it and probing again can mistake our looped-back goodbye
    // (or an old cached answer from another browser) for a name conflict.
    if(pub->mode == JDNS_PUBLISH_SHARED)
    {
        mdnsd_done(s->mdns, r);
        r = mdnsd_shared(s->mdns, (char *)pub->rr->owner, pub->rr->type, rr->ttl);
    }
    pub->rec = r;
    mdnsd_set_ttl(r, rr->ttl);

    if(!_publish_applyrr(s, r, rr))
    {
        _debug_line(s, "attempt to update_publish an unsupported type");
        return;
    }
    jdns_rr_delete(pub->rr);
    pub->rr = jdns_rr_copy(rr); // interface refreshes must retain the new value
}

void _multicast_cancel_publish(jdns_session_t *s, int id)
{
    int n;
    for(n = 0; n < s->published->count; ++n)
    {
        published_item_t *i = (published_item_t *)s->published->item[n];
        if(i->id == id)
        {
            mdnsd_done(s->mdns, i->rec);
            list_remove(s->published, i);
            break;
        }
    }
}

void _multicast_flush(jdns_session_t *s)
{
    int n;

    // to flush, we make like our queries and published items are all new.
    // we'll do this by destroying/creating the mdnsd object again (so it
    // is fresh) and then reapply all queries and published items to it.

    // start over with mdnsd
    mdnsd_free(s->mdns);
    s->mdns = mdnsd_new(0x0001, 1000, s->port, _callback_time_now, _callback_rand_int, s);

    // attempt to publish again
    for(n = 0; n < s->published->count; ++n)
    {
        published_item_t *i;
        mdnsdr r;

        i = (published_item_t *)s->published->item[n];
        if(i->mode == JDNS_PUBLISH_UNIQUE)
            r = mdnsd_unique(s->mdns, (char *)i->rr->owner, i->rr->type, i->rr->ttl, _multicast_pubresult, s);
        else
            r = mdnsd_shared(s->mdns, (char *)i->rr->owner, i->rr->type, i->rr->ttl);
        _publish_applyrr(s, r, i->rr);
        i->rec = r;
    }

    // restore the queries
    for(n = 0; n < s->queries->count; ++n)
    {
        query_t *q = (query_t *)s->queries->item[n];

        // issue the query
        mdnsd_query(s->mdns, (char *)q->qname, q->qtype, _multicast_query_ans, s);
    }
}

int jdns_step_multicast(jdns_session_t *s, int now)
{
    int need_read, need_write, smallest_time;
    struct mytimeval *tv;
    jdns_packet_t *packet;
    int flags;

    // not used
    (void)now;

    need_read = 0;
    need_write = 0;

    if(s->shutdown == 1)
        mdnsd_shutdown(s->mdns);

    while(1)
    {
        jdns_address_t *addr;
        unsigned short int port;
        int ret;
        unsigned char *buf;
        int buf_len;

        /* Do not dequeue/serialize an mDNS message while output is blocked.
         * mdnsd_out() consumes pending work and allocates a packet, so calling
         * it before checking writability both leaked memory and lost packets. */
        if(!s->handle_writable)
        {
            need_write = 1;
            break;
        }
        if(!mdnsd_out(s->mdns, &packet, &addr, &port))
            break;

        if(!jdns_packet_export(packet, JDNS_UDP_MUL_OUT_MAX))
        {
            _debug_line(s, "outgoing packet export error, not sending");
            jdns_packet_delete(packet);
            continue;
        }

        buf = packet->raw_data;
        buf_len = packet->raw_size;

        // multicast
        if(!addr)
        {
            addr = jdns_address_copy(s->maddr);
            port = s->port;
        }

        _debug_line(s, "SEND %s:%d (size=%d)", addr->c_str, port, buf_len);
        _print_hexdump(s, buf, buf_len);

        ret = s->datagram.transport_write(s, s->cb.app, s->handle, addr, port, buf, buf_len);

        jdns_address_delete(addr);
        jdns_packet_delete(packet);

        // if we can't write the packet, oh well
        if(ret == 0)
        {
            s->handle_writable = 0;
            need_write = 1;
            break;
        }
    }

    if(s->shutdown == 1)
    {
        jdns_event_t *event = jdns_event_new();
        event->type = JDNS_EVENT_SHUTDOWN;
        _append_event(s, event);
        s->shutdown = 2;
        return 0;
    }

    // let's always ask for reads, just so the user doesn't have to
    //  worry about what should happen to incoming packets otherwise
    need_read = 1;

    if(s->handle_readable)
    {
        while(1)
        {
            unsigned char buf[JDNS_UDP_MUL_IN_MAX];
            int bufsize = JDNS_UDP_MUL_IN_MAX;
            int ret;
            jdns_address_t *addr;
            int port;
            jdns_response_t *r;

            addr = jdns_address_new();
            ret = s->datagram.transport_read(s, s->cb.app, s->handle, addr, &port, buf, &bufsize);

            // no packet?
            if(ret == 0)
            {
                s->handle_readable = 0;
                jdns_address_delete(addr);
                break;
            }

            _debug_line(s, "RECV %s:%d (size=%d)", addr->c_str, port, bufsize);
            _print_hexdump(s, buf, bufsize);

            if(!jdns_packet_import(&packet, buf, bufsize))
            {
                _debug_line(s, "error parsing packet / too large");

                jdns_address_delete(addr);
                continue;
            }

            _print_packet(s, packet);

            r = _packet2response(packet, 0, 0, 0x7fff);
            _print_records(s, r, 0);

            mdnsd_in(s->mdns, packet, r, addr, (unsigned short)port);

            jdns_address_delete(addr);
            jdns_packet_delete(packet);
            jdns_response_delete(r);
        }
    }

    tv = mdnsd_sleep(s->mdns);
    smallest_time = tv->tv_sec * 1000 + tv->tv_usec / 1000;

    flags = 0;
    if(smallest_time != -1)
    {
        flags |= JDNS_STEP_TIMER;
        s->next_timer = smallest_time;

        // offset it a little bit, so that the user doesn't call
        //  us too early, resulting in a no-op and another timer
        //  of 1 millisecond.
        s->next_timer += 2;
    }
    if(need_read || need_write)
        flags |= JDNS_STEP_HANDLE;
    return flags;
}
