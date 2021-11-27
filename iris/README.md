# Iris XMPP Library

## What is it?

Iris is a comprehensive Qt/C++ library for working with the XMPP protocol. It complies with the XMPP RFCs and many XEPs. It should be useful for creating clients, servers, and other components.

In addition to XMPP, Iris also offers access to many other protocols and concepts, including DNS, DNS-SD, connection and transport abstraction, HTTP, SOCKS, XML streaming, network interfaces, compression, encryption, authentication, flow control, proxies, and NAT traversal.

## License

This library is licensed under the Lesser GNU General Public License. See the COPYING file for more information.

## What do I need to be able to use it?

Iris depends on Qt and QCA.

## What features are supported?

* Full support for draft-ietf-xmpp-core-21, including:
  * SRV DNS lookups
  * SSL/TLS security (both old way and ‘STARTTLS’ variations)
  * SASL authentication/encryption
  * Older jabber:iq:auth login
  * Resource binding
  * HTTP Connect, SOCKS5, and HTTP Polling proxy support
  * High-level objects for dealing with Stanzas and Streams

* Parts of xmpp-im:
  * Message exchange
  * Presence broadcast / reciept
  * Roster management
  * Subscriptions

* XEP extensions:
  * Version/Time requests
  * Service Discovery (disco, browse, and older ‘agents’ modes)
  * Account registration
  * Password changing
  * Agent/Transport registration
  * vCards
  * Basic Groupchat
  * OpenPGP capable
  * S5B Direct Connections
  * File Transfer

* And probably more things I forgot to mention…

## Install

First, build Iris:

Unix:

```sh
./configure
make
```

Windows:

```
copy conf_win.pri.example conf_win.pri
copy confapp_win.pri.example confapp_win.pri
qmake
make (or nmake)
```

There is no installation. Just include iris.pri in your qmake project. Iris requires Qt 4.2 or greater and QCA 2.0 or greater.

## How does it work?

(TODO)

## What is the development plan?

1. Finish basic server support for xmpp-core
2. Ensure xmpp-core fully matches the draft
3. Write API docs for xmpp-core
4. Write full xmpp-im API
5. Additional important specs: x:data, MUC, etc
6. ...

## What's up with the name 'Iris'?

Iris is the Greek goddess of messaging and the rainbow. She works part-time delivering XML.
