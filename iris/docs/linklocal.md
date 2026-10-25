# XEP-0174 link-local example

`iris-linklocal` demonstrates the JDNS 3.1 multicast API with actual serverless
XMPP messages. Protocol references: [XEP-0174](https://xmpp.org/extensions/xep-0174.html),
[RFC 6762](https://www.rfc-editor.org/rfc/rfc6762.html) and
[RFC 6763](https://www.rfc-editor.org/rfc/rfc6763.html).

Build with either supported Qt major:

```sh
cmake -S . -B build-linklocal -DUSE_QT6=ON -DIRIS_BUILD_TESTS=ON \
  -DIRIS_BUILD_LINKLOCAL_EXAMPLE=ON -DIRIS_ENABLE_OMEMO=OFF \
  -DIRIS_SYSTEM_JDNS=OFF
cmake --build build-linklocal --target iris-linklocal linklocal_stream_test -j8
ctest --test-dir build-linklocal --output-on-failure -R '^linklocal_'
```

For Qt 5 use `-DUSE_QT6=OFF`. Unlike the historical `IRIS_BUILD_TOOLS` switch,
this example also builds with a system QCA installation. Use the usual QCA
selection options for the platform. JDNS comes from the dependency lock, or a
matching Qt build of system QJDns 3.1+ selected with `IRIS_SYSTEM_JDNS=ON`.

Two terminals on the same machine (loopback multicast must be permitted):

```sh
build-linklocal/tools/linklocal/iris-linklocal --allow-plain --interface lo \
  --jid juliet@pronto --echo
```

```sh
build-linklocal/tools/linklocal/iris-linklocal --allow-plain --interface lo \
  --jid romeo@forza --send juliet@pronto --message 'Hello' --expect 'Hello' \
  --exit-after-message --run-for 15000
```

Use a real interface name to communicate between two LAN hosts; `--address`
selects a specific local IPv4/IPv6 address instead. IPv6 link-local addresses
must retain their interface scope, e.g. `fe80::1234%eth0`. Ctrl+C/SIGTERM performs
the XML close handshake and sends mDNS goodbyes before exiting.

The example:

- Opens TCP on a dynamically allocated port on the selected local address.
- Publishes unique A/AAAA, SRV and TXT records, then the shared presence PTR.
- Renames conflicting host/user names with numeric suffixes before reannouncing.
- Browses PTR continuously; logical identities appear once in its single-link roster.
- Resolves SRV/TXT and addresses only when communicating, uses the SRV port,
  cancels detail subscriptions, and retains no endpoint IP cache.
- Accepts empty TXT metadata (`--empty-txt`), and does not use optional fields
  as a prerequisite for connecting.
- Exchanges message/IQ stanzas through `XMPP::LinkLocalStream`, the existing
  Iris `Parser` and `Stanza` API. Presence stays in DNS-SD, not XML presence.
- Checks stanza addresses against stream endpoints, rejects restricted XML,
  bounds incomplete input and queues, and mirrors stream close tags while
  accepting in-flight messages.

The standalone two-process regression runs real UDP multicast and TCP:

```sh
python3 tests/linklocal/mdns_tcp.py build-linklocal/tools/linklocal/iris-linklocal
```

It verifies discovery, SRV resolution, exact message/echo bodies, stream teardown
and disappearance after a PTR goodbye, both with normal and empty TXT metadata.
It requires local sockets, not an internet XMPP server or Docker. The separate
stream tests cover fragmented XML, legacy headers, address injection, malformed
XML/DTDs, required TLS rejection and consumer deletion during callbacks.

## Current scope

This is an opt-in protocol example and an initial reusable `LinkLocalStream`,
not a Psi account type or a complete serverless roster implementation. It uses
one network link/address per process. Multiple-link roster merging, interface
change handling, avatars, capabilities and the Psi UI are subsequent application
work. The generic JDNS API keeps per-link sessions independent.

TLS/SASL negotiation is not implemented. `--allow-plain` explicitly enables
unauthenticated, unencrypted TCP for this example. Matching header addresses
prevents third-party stanza addressing but does not authenticate a peer. A peer
requiring STARTTLS is rejected before messages are sent. Production integration
needs an explicit security policy and TLS/authentication support; this example
does not advertise unsupported features or downgrade a required TLS stream.

## Reusable account transport

`LinkLocalService` combines interface-scoped discovery, DNS-SD presence and peer
TCP streams into a `Stream` for one local account. It reuses `QJDnsMulticast` and
`LinkLocalStream`; the GUI need not implement DNS records, peer lookup or framing.
Connect signals before calling `start()`, and explicitly opt into the current
unauthenticated/plaintext transport at the application level.

```cpp
XMPP::Client client;
auto *service = new XMPP::LinkLocalService(&client);
client.connectToStream(service, XMPP::Jid("alice@laptop"));
connect(service, &XMPP::LinkLocalService::online, &client, [&client] {
    client.setPresence(XMPP::Status(XMPP::Status::Online));
});
service->start(XMPP::Jid("alice@laptop"), selectedLocalAddress, "Alice");
```

`Client::connectToStream()` attaches an externally owned non-server transport and
retains the normal message/IQ tasks and encryption/plugin hooks. It does not log in,
request a roster or bind a server resource. The service's `peerChanged()` signals
update Client's local roster and presence; `setPresence()` updates TXT instead of
sending XML presence. Peer failures use `peerError()` and do not disconnect the
whole account. `close()` withdraws records and completes the XML close handshakes.
The service bounds connections, pending stanzas, received stanzas and input bytes.

JDNS is enabled by `IRIS_ENABLE_JDNS`, Android, or the link-local example option.
Without it the API remains linkable, `isSupported()` is false, and `start()` reports
a deferred error. One explicit local address is used per service; TLS/SASL and
multi-link aggregation are outside this API's current implementation.
