# RFC 8765 readiness: DNS Push Notifications

## Standards and scope

RFC 8765 (https://www.rfc-editor.org/rfc/rfc8765) adds long-lived
**unicast** subscriptions to changing DNS RRsets. It does not change RFC
6762 multicast DNS. DNS Push uses DNS Stateful Operations (RFC 8490) over
TLS/TCP (RFC 7858), with server discovery, keepalives, subscription
acknowledgement and asynchronous update delivery.

Today JDNS implements one-shot unicast lookups and RFC 6762-style multicast
subscriptions/publications. It does **not** implement DSO or RFC 8765.
Merely extracting the multicast source file does not claim compliance.

## Code boundaries after PR #17

- `jdns.c`: ordinary unicast query state, wire encoding/decoding, CNAME,
  TTL cache, concurrent consumer coalescing, and typed I/O errors.
- `jdns_multicast.c`: multicast query subscriptions, known-answer snapshots,
  publication/conflict handling, probe/flush, and packet delivery.
- `jdns_mdnsd.c`: low-level multicast scheduling/cache/probing.
- `QJDnsQueryTransport`: independent, one-shot unicast request/response I/O.
- `QJDnsDatagramTransport`: real datagram socket operations, including mDNS.

`jdns_core_p.h` is a **private compilation boundary**, not a public API.
Current multicast still shares portions of the internal session/ID/event
structures. Future subscription state must not be grafted onto the existing
`query_t::step` or `query_t::mul_known` fields.

## Recommended eventual DNS Push design

### 1. Persistent session, not another request transport

Introduce a separate `jdns_push_session`/DSO component with a long-lived
TLS/TCP connection abstraction. DNS Push cannot correctly be implemented
using one `startRequest()` followed by one `responseReady()`: a single
subscription receives **multiple** updates, unsolicited DSO messages,
keepalives and potentially reconnect/resubscribe events.

Keep DSO wire framing/TLV parsing separate from the normal DNS response
parser. DSO uses DNS opcode 6 and DNS Stateful Operations, not an ordinary
DNS Query/Response message. No need to modify the mDNS engine for DSO.

### 2. Explicit subscription identity and lifecycle

Expose future subscription operations separately from ordinary queries:
subscribe(owner, rrtype, rrclass), unsubscribe(subscriptionId), and
change/error events. Track three identities independently:
- logical consumer subscriptions (multiple callers may subscribe to same RRset);
- remote DSO subscription state, ID and outstanding request;
- underlying persistent connection (shared by subscriptions to the same
  trusted server).

Cancel after the last consumer unsubscribes. Ignore late pushes for removed
subscriptions, as required by RFC 8765. Preserve subscribe and resubscribe
state across connection drop with explicit re-establishment, backoff and
clear error reporting. Do not create idle subscriptions just to warm caches.

### 3. RRset change semantics

A push notification is **not always a snapshot**. Model typed changes
(add/replace record, delete a record, delete the entire RRset, delete all
RRsets for a name/class) separately from full one-shot DNS responses.
RFC 8765 distinguishes ordinary TTL-zero deletes from the special
TTL=0xFFFFFFFE with RDLEN=0 and wildcard TYPE/CLASS deletes.
Applying those updates directly through the current CNAME/TTL cache as if
they were regular DNS replies would produce incorrect results.

A future common record-store interface may be shared by mDNS and DNS Push;
the two protocol engines should independently translate wire updates into
explicit record-change events. Avoid introducing that shared store until
change semantics and test vectors are defined.

### 4. Discovery, authentication, and routing

Use existing unicast SRV lookups to discover `_dns-push-tls._tcp.<zone>`
when direct authoritative-server discovery is needed. RFC 8765 also
specifies starting with the configured recursive resolver's DSO/DoT
service on TCP 853, with fallback to explicit authoritative discovery
when unsupported.

TLS verification is mandatory, with appropriate authentication of the
server name and DANE TLSA where applicable as described in the RFC.
Proxy policy and trust settings belong to the caller's session/transport
configuration. Never silently fall back to a local resolver, plaintext
TCP, untrusted TLS, or multicast upon Push subscription failure.

### 5. Boundary of responsibilities

The unicast resolver may resolve discovery SRVs and parse RRsets.
A future `jdns_push.c` owns subscriptions, DSO session state, keepalives,
server acknowledgements, reconnection and update decoding.
A future session-oriented Qt interface owns a persistent TLS connection.
The mDNS module continues to handle link-local multicast and publication,
with no knowledge of DSO or remote unicast Push servers.

## Required tests before implementing RFC 8765

- DSO opcode 6, malformed primary/additional TLVs, unknown TLV handling
  and length/buffer boundaries.
- SUBSCRIBE accept/reject (including DSOTYPENI), duplicate subscriptions,
  multi-consumer coalescing, UNSUBSCRIBE race with in-flight push.
- Record adds, TTL-zero individual delete, RRset delete,
  name/class delete, CNAME changes and cache invalidation.
- Keepalive, inactivity, update ordering, reconnect/resubscribe, failover,
  cancellation and late messages.
- Authentication failure, proxy routing, strict TLS requirement and
  absence of insecure fallback.
- Interoperation with an RFC 8765 reference implementation where available.

## Deliberately out of scope for this refactor

No RFC 8765/DSO framing or subscription implementation is added in PR #17.
This refactor only ensures that multicast protocol state is not coupled to
one-shot unicast query transport semantics, and records the independent
session-oriented boundary future DNS Push support requires.
