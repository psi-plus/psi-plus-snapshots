#!/usr/bin/env python3
"""Check the Linux shared ABI without inspecting the Iris source/build tree."""

import re
import subprocess
import sys

symbols = subprocess.check_output(
    ["nm", "-D", "--defined-only", "--demangle", sys.argv[1]], text=True
)
names = {line.split(" ", 2)[-1] for line in symbols.splitlines()}
required = {
    "typeinfo for ByteStream",
    "BSocket::address() const",
    "XMPP::ProcessQuit::instance()",
    "XMPP::Dtls::supportedSRTPProfiles()",
    "XMPP::irisNetCleanup()",
}
missing = required - names
if missing:
    sys.exit("Missing public exports: " + ", ".join(sorted(missing)))

# Pimpls inherit their enclosing class's visibility unless explicitly hidden.
# Keep both those and non-installed irisnet implementation classes out of ABI.
public_with_pimpl = (
    "AddressResolver|NetAvailability|NameRecord|ServiceInstance|NameResolver|"
    "ServiceBrowser|ServiceResolver|ServiceLocalPublisher|Dtls|Ice176|IceAgent|"
    "IceComponent|ProcessQuit|StunAllocate|StunBinding|StunMessage|TcpPortScope|"
    "TurnClient|UdpPortReserver|ByteStream|BSocket|HttpConnect|HttpPoll|"
    "SocksUDP|SocksClient|SocksServer|SrvResolver"
)
private = re.compile(
    rf"\b(?:{public_with_pimpl})::Private\b|"
    r"\b(?:ObjectSessionPrivate|ObjectSessionWatcherPrivate|NameManager|"
    r"NetInterfaceManagerPrivate|NetInterfacePrivate|IceLocalTransport|"
    r"IceTurnTransport|StunTransactionPrivate|StunTransactionPoolPrivate|"
    r"HttpProxyPost|HttpProxyGetStream)\b|"
    r"\bXMPP::irisNet(?:AddPostRoutine|Providers)\("
)
leaked = sorted(name for name in names if private.search(name))
if leaked:
    sys.exit("Private irisnet exports:\n" + "\n".join(leaked))
print("Public irisnet exports present; checked private implementation symbols hidden")
