# JDNS

JDNS is an event-loop-friendly DNS library with a dependency-free C protocol
engine and a Qt wrapper, **QJDns**, for Qt 5 and Qt 6. It supports traditional
DNS resolution and multicast DNS (mDNS), including publishing records on the
local network. Originally created by Justin Karneges in 2005, it is licensed
under the MIT license.

## What it can do

| Capability | Details |
| --- | --- |
| Unicast DNS | Asynchronous queries, including A, AAAA, SRV, MX, TXT, PTR, CNAME and other DNS record types (unknown types can be handled as raw RDATA). |
| DNS resolution engine | DNS wire encoding/decoding, CNAME traversal, TTL caching/expiration and coalescing of identical concurrent queries. |
| Multicast DNS | mDNS queries, ongoing discovery, record publication, updates and conflict handling using the bundled `mdnsd` engine. |
| UDP | Built-in datagram transport for conventional DNS and mDNS. |
| DNS over TCP | `QJDnsTcpTransport` implements DNS-over-TCP framing (RFC 7766), with explicit Qt proxy configuration, including SOCKS5. |
| DNS over HTTPS | `QJDnsHttpTransport` implements RFC 8484 using Qt Network. **HTTPS is required**; plain HTTP and redirects are rejected. Its `QNetworkAccessManager` may be supplied or internally owned. |
| Custom backends | `QJDnsExternalTransport` passes complete DNS wire requests to an application-owned asynchronous backend (for example, a custom HTTP/TLS stack). |
| Android DNS | A platform-specific backend uses the Android system resolver where available, taking account of the platform's network and Private DNS configuration; older Android versions use a fallback resolver implementation. |
| Error and cancellation handling | Distinct transport failures/timeouts, per-request cancellation and no implicit fallback to a different DNS transport after an error. |

The C API supplies callbacks for the application's clock, randomness and
network I/O; it has no Qt dependency. The Qt wrapper supplies the event-loop
integration and the concrete transport implementations.

**Scope:** JDNS is a DNS/mDNS building block, not a complete DNS-SD/Bonjour
service-discovery framework. The application handles service browsing policy,
search-domain expansion and selection between unicast DNS and `.local` mDNS.
**DNS Push Notifications (RFC 8765) are not implemented.** Their planned,
separate, session-oriented design is discussed in
[docs/rfc8765-readiness.md](docs/rfc8765-readiness.md).

## Build

CMake 3.19 or newer is required. The following builds the C library, Qt 6
wrapper and command-line utility (where supported):

```sh
cmake -S . -B build -DBUILD_QJDNS=ON -DJDNS_QT_MAJOR_VERSION=6
cmake --build build --parallel
```

Set `JDNS_QT_MAJOR_VERSION=5` for Qt 5. Set `BUILD_QJDNS=OFF` to build
only the C library, `BUILD_JDNS_TOOL=OFF` to omit the example CLI, or
`BUILD_SHARED_LIBS=ON` to build shared libraries (static is the default).

To run the regression suite:

```sh
cmake -S . -B build -DJDNS_BUILD_TESTS=ON -DJDNS_QT_MAJOR_VERSION=6
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

You can install the libraries and CMake package configuration with
`cmake --install build`.

## QJDns 3.0 factories and object requests

Every `create()` returns an initialized, ready-to-query resolver or `nullptr`
on configuration/initialization failure. Network failures are asynchronous.
All objects must be used on their Qt thread. The system factory uses the OS
DNS configuration (UDP on desktop, the platform resolver on Android):

```cpp
QJDns *system = QJDns::create(parent);
QJDns *https = QJDns::create(QUrl("https://cloudflare-dns.com/dns-query"), parent);
QJDns *tcp = QJDns::create(QUrl("tcp://1.1.1.1"),
    QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", 9050), parent);
QJDns *customHttp = QJDns::create(QUrl("https://example.org/dns-query"), nam, parent);
QJDns *customBackend = QJDns::create(transport, parent);
```

URL factories follow Qt's application proxy policy by default; pass an explicit
`QNetworkProxy` to select another policy. The HTTPS factory creates and owns its
`QNetworkAccessManager` unless one is supplied. A supplied manager stays
caller-owned and must outlive outstanding requests. The transport factory
**always takes ownership**, including on failure. Its transport must be a
fresh heap object, not shared with another resolver.

Only HTTPS and TCP URLs are accepted. User information, fragments, port zero,
and TCP paths/queries are rejected; HTTPS paths and queries are preserved.
There is no implicit local DNS fallback after a remote failure. Qt may resolve
the resolver endpoint's hostname locally unless its proxy handles that lookup.

### Complete example: resolve an A record via HTTPS

```cpp
// main.cpp
#include <QCoreApplication>
#include <QDebug>
#include <qjdns.h>
#include <qjdnsshared.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto *dns = QJDns::create(QUrl("https://dns.google/dns-query"), &app);
    if (!dns)
        return 1;
    auto *request = dns->query("example.org.", QJDns::A, &app);
    QObject::connect(request, &QJDnsSharedRequest::resultsReady, &app, [request, &app] {
        if (!request->success()) {
            qWarning() << "DNS lookup failed:" << request->error();
            app.exit(1);
            return;
        }
        for (const auto &record : request->results()) {
            if (record.type == QJDns::A)
                qInfo().noquote() << record.address.toString();
        }
        app.quit();
    });
    return app.exec();
}
```

For an installed Qt 6 package:

```cmake
cmake_minimum_required(VERSION 3.19)
project(dns_example LANGUAGES CXX)
set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt major version")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Network)
find_package(QJDns 3.0 CONFIG REQUIRED)
add_executable(dns_example main.cpp)
target_link_libraries(dns_example PRIVATE QJDns::QJDns)
```

Pass `-DQT_DEFAULT_MAJOR_VERSION=5` to build the example with Qt 5.

`QJDns::query()` always returns a **pending** `QJDnsSharedRequest`: its start
and completion happen on the event loop, including cache hits and validation
errors. Connect immediately after the call, before processing events. Each
call creates a separate request object; JDNS coalesces identical DNS work
internally. Deleting one request does not cancel other consumers. Deleting or
cancelling before start prevents network I/O. Explicit `cancel()` reports
`ErrorCancelled` asynchronously once; deletion stays silent. Reusing a request
with `query()` silently replaces its previous operation, including queued work.
A completed request retains its result until deletion. The default request
parent is its resolver; a supplied parent may outlive the resolver, in which
case an active request reports `ErrorResolverDestroyed` asynchronously.

### Compatibility

The released 2.0.6 constructor, `init(mode, bindAddress)`, explicit
`setNameServers()`, ID-based `queryStart()`/`queryCancel()`, multicast API and
`QJDnsShared`/`QJDnsSharedRequest` API remain available. Legacy
`QJDnsSharedRequest::cancel()` keeps its silent behavior. The new single-session
request constructor and `QJDns::query()` are for unicast queries; legacy
multicast discovery/publication continues through `QJDnsSharedRequest` with a
`QJDnsShared` resolver.

Unreleased `setQueryTransport()`, `setDatagramTransport()` and `init(mode)`
were replaced by the factories. The datagram transport is now private; public
query transports remain available for TCP, HTTPS and external backends.
Version 3.0 uses SOVERSION 3 and same-major CMake package compatibility.
The additive multicast convenience API below is new in 3.1.

## QJDns 3.1 multicast subscriptions and publications

Use `QJDnsMulticast` for an explicitly selected network link. Its factories
return a ready session or `nullptr`; they do not silently use another interface.
The interface factory opens IPv4/IPv6 where available. The address factory selects
one family on the interface owning a concrete local address (IPv6 link-local
addresses accept an interface name or index as their scope ID).

```cpp
#include <qjdnsmulticast.h>

auto *mdns = QJDnsMulticast::create(QNetworkInterface::interfaceFromName("eth0"), parent);
if (!mdns)
    return;
auto *browse = mdns->subscribe("_presence._tcp.local.", QJDns::Ptr);
QObject::connect(browse, &QJDnsSharedRequest::resultsReady, parent, [browse] {
    if (!browse->success())
        return; // inspect browse->error() in the application
    for (const auto &change : browse->results()) {
        const auto user = QJDnsMulticast::instanceName(change.name, "_presence._tcp");
        // change.ttl > 0: available; change.ttl == 0: removed.
        // results() is a batch of changes, not a complete roster snapshot.
    }
});

QJDns::Record host;
host.owner = "pronto.local.";
host.type = QJDns::A;
host.haveKnown = true;
host.ttl = 120;
// Unspecified address: use this session's IPv4 source address.
auto *publication = mdns->publish(host);
QObject::connect(publication, &QJDnsSharedRequest::resultsReady, parent, [publication] {
    // success(): unique record has completed probing; ErrorConflict: rename it.
});
// publication->publishUpdate(updatedRecord);
// mdns->unpublish(publication);  // or publication->cancel()/delete publication
```

Every call returns a separate, caller-deletable `QJDnsSharedRequest`, owned by
the session unless a parent is supplied. Its start, cached answers and validation
errors are deferred until the event loop, leaving time to connect signals.
Cancel/delete before start prevents network I/O. Multicast cancellation stays
silent. Subscriptions and publications are long-lived; a published unique name
can still report a later conflict. Requests with an external parent safely report
`ErrorResolverDestroyed` if the session is destroyed.

Call `shutdown()` from the application's stop handler, then wait for
`shutdownFinished()` before deleting the session or exiting the event loop.
This sends goodbye records. Immediate destruction does not wait for I/O.
Recreate a session when its interface/address changes. Keep interface scope with
discovered endpoints; `.local.` names on separate links are independent.
Multicast uses direct local UDP, independently of the application's proxy.
Firewalls and platform multicast permissions (including Android Wi-Fi multicast
reception) remain the application's responsibility.

`serviceName()` encodes an NFC UTF-8 DNS-SD instance as one label, escaping literal
dots/backslashes. `instanceName()` reverses it. Neither applies IDNA to a user
display name. Host names and service type selection remain application policy.
The generic API does not embed XMPP presence or a second DNS-SD cache. The Iris
`iris-linklocal` example demonstrates `_presence._tcp.local.` publication,
on-demand resolution and XEP-0174 messaging using this API.

## Using the pure C core

The core API is in `include/jdns/jdns.h`. Create a session with
`jdns_session_new()` and common clock/random/debug callbacks. Initialize
either datagram DNS with `jdns_init_unicast()`, native async unicast with
`jdns_init_async_unicast()`, or mDNS with `jdns_init_multicast()`. Issue
queries through `jdns_query()`, drive timers and events via `jdns_step()`,
and dispose of the session using `jdns_session_delete()`.

The async unicast callback receives a complete DNS wire query and an internal
request ID; the application reports a response through
`jdns_async_complete()` or a typed failure through `jdns_async_fail()`.
Datagram callbacks instead implement actual socket binding, sending and
receiving. The two callback tables are intentionally separate. See the public
header's Doxygen comments for lifetime, threading/reentrancy and cancellation
contracts.
