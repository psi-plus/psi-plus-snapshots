# JDNS native asynchronous unicast architecture

## Separation of concerns

The DNS engine encodes/decodes packets, traverses CNAMEs, caches TTL records
and coalesces identical lookups. Network transports perform routing, retries,
timeouts and cancellation. There is never an implicit fallback to another DNS
server, UDP or local system DNS after a native transport failure.

**Datagram transport** (QJDnsDatagramTransport): real UDP socket binding,
multicast membership, source/destination endpoint and packet I/O.
**Query transport** (QJDnsQueryTransport): one complete DNS wire request,
its corresponding response or typed error, and cancellation. No bind
address, artificial socket handle, packet sender or destination parameter.

## Native C API

jdns_init_async_unicast(session) requires async_send(session, user, id, wire)
and async_cancel(session, user, id). Network results arrive through
jdns_async_complete(session, id, wire) or jdns_async_fail(session, id, status).
Neither method creates a fabricated SERVFAIL DNS reply.

The id is the internal **wire-query ID**, distinct from consumer request IDs
and the 16-bit DNS transaction ID. The engine validates all three relevant
identifiers/question contents, ignoring stale or mismatched replies.

One cancelled consumer does not stop a coalesced query if others need it.
Cancelling the final consumer aborts transport I/O immediately. Native async
queries are not retained in the background just to populate the cache.
Shutdown cancels any outstanding transport operations.

Native async submission encodes a DNS message and calls async_send directly:
it never allocates a fake socket or enqueues a fake datagram.

## Qt API and ownership

Call QJDns::create(transport, parent). The resolver always takes ownership
of the transport, including on initialization failure. The transport implements startRequest,
cancelRequest, and emits responseReady or failed. Callback completion must
never synchronously reenter the JDNS stepping loop.

QJDnsDatagramTransport is a private implementation detail for real datagram
socket implementations. The platform's ordinary UDP/mDNS stack remains
the default where appropriate. Android uses its network-aware system
resolver and Private DNS through the native query interface.

The included query transports are:
- QJDnsTcpTransport: RFC 7766; explicit server, port and optional Qt proxy.
- QJDnsHttpTransport: RFC 8484; caller-owned QNAM or default owned QNAM.
- QJDnsExternalTransport: HTTP/TLS-agnostic asynchronous external backend.

A default-created QNAM uses normal Qt proxy settings; it cannot infer
per-connection proxy policy from an unrelated application component.
Network failure or timeout does not trigger local DNS fallback.

QJDns::setNameServers() explicitly returns false for query transports,
because server selection belongs to the transport.

## Regression coverage

C tests (GCC and Clang, Debug and Release, without Qt): successful A,
cache hit and expiry, CNAME success and child error, NXDOMAIN, two
coalesced consumers, cancellation before dispatch and on last consumer,
invalid and spoofed DNS responses, late/duplicate completion, typed
timeout/network failure and cancellation on destruction.

Qt tests: loopback UDP, TCP framing including fragmented replies, SOCKS5
hostname CONNECT, RFC 8484 DoH, owned and external QNAM, HTTP errors,
explicit external backend, timeout, malformed response and cancellation.

Android CI must continue validating native resolver availability and
Private DNS and network-binding paths. No mDNS API changes are required
for the query transport split.

## Remaining internal modularization

Multicast-specific state and its real datagram callbacks still reside
alongside unicast state within the C translation unit. Physically moving
the mDNS engine into its own module remains a separate internal step.
It must not reintroduce fake datagram semantics into query transports.

## Object requests

QJDns::query returns an independent QJDnsSharedRequest, reusing the released
request class rather than introducing another subscription abstraction. A
generation counter retires queued starts when the request is cancelled, reused
or destroyed. A request is started only after the caller can connect signals.
Each low-level query ID identifies one consumer; network coalescing remains
inside JDNS. Terminal state and results are stored before completion delivery,
so they remain readable after the resolver is gone.
