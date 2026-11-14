#!/usr/bin/env python3
"""Check Doxygen contracts on the public DNS transport APIs.

This is deliberately a lightweight, dependency-free lint for the API split:
keep its symbol inventory in sync when adding/changing public operations.
It verifies a preceding Doxygen block and documentation of all parameters.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = {
    "include/jdns/qjdnsmulticast.h": [
        "create", "~QJDnsMulticast", "networkInterface", "addresses", "serviceName", "instanceName",
        "subscribe", "publish", "unpublish", "shutdown", "shutdownFinished",
    ],
    "include/jdns/jdns.h": [
        "jdns_session_new", "jdns_session_delete", "jdns_init_unicast",
        "jdns_init_multicast", "jdns_init_async_unicast",
        "jdns_async_complete", "jdns_async_fail", "jdns_set_nameservers",
    ],
    "include/jdns/qjdns.h": [
        "create", "query", "setNameServers",
        "queryStart", "queryCancel", "publishStart", "publishUpdate",
        "publishCancel", "shutdown",
    ],
    "include/jdns/qjdns_querytransport.h": [
        "startRequest", "cancelRequest", "responseReady", "failed", "debugLine",
    ],
    "include/jdns/qjdns_externaltransport.h": [
        "QJDnsExternalTransport", "~QJDnsExternalTransport", "setTimeout",
        "startRequest", "cancelRequest", "completeRequest", "failRequest",
        "queryRequested", "requestCancelled",
    ],
    "include/jdns/qjdns_tcptransport.h": [
        "QJDnsTcpTransport", "~QJDnsTcpTransport", "setTimeout",
        "startRequest", "cancelRequest",
    ],
    "include/jdns/qjdns_httptransport.h": [
        "QJDnsHttpTransport", "~QJDnsHttpTransport", "setTimeout",
        "networkAccessManager", "startRequest", "cancelRequest",
    ],
}
C_CALLBACKS = {
    "time_now", "rand_int", "debug_line",
    "transport_bind", "transport_unbind", "transport_read", "transport_write",
    "send", "cancel",
}

def erased(text):
    pattern = r"/\*[\s\S]*?\*/|//[^\n]*"
    return re.sub(pattern, lambda m: re.sub(r"[^\n]", " ", m.group()), text)

def parameters(signature):
    args = []
    for segment in signature.split(","):
        segment = segment.split("=")[0].strip()
        if not segment or segment == "void":
            continue
        m = re.search(r"([A-Za-z_]\w*)\s*$", segment)
        if m:
            args.append(m.group(1))
    return args

errors = []
checked = 0
for relative, names in PUBLIC.items():
    text = (ROOT / relative).read_text(encoding="utf-8")
    source = erased(text)
    for name in names:
        if relative.endswith("/jdns.h") and name in C_CALLBACKS:
            continue
        symbol = re.escape(name)
        match = re.search(r"(?<![\w])" + symbol + r"\s*\(([^;{}()]*)\)", source)
        if not match:
            errors.append(f"{relative}: declaration for {name} not found")
            continue
        checked += 1
        prior = text.rfind("*/", 0, match.start())
        first = text.rfind("/**", 0, prior + 1)
        if first < 0 or prior < 0:
            errors.append(f"{relative}: {name}: missing Doxygen comment")
            continue
        gap = source[prior + 2 : match.start()]
        if ";" in gap or "}" in gap or len(gap) > 180:
            errors.append(f"{relative}: {name}: Doxygen not adjacent to declaration")
            continue
        doc = text[first:prior + 2]
        for parameter in parameters(match.group(1)):
            if not re.search(r"@param\s+" + re.escape(parameter) + r"\b", doc):
                errors.append(f"{relative}: {name}: missing @param {parameter}")

relative = "include/jdns/jdns.h"
text = (ROOT / relative).read_text(encoding="utf-8")
source = erased(text)
for name in sorted(C_CALLBACKS):
    match = re.search(
        r"\(\*\s*" + re.escape(name) + r"\s*\)\s*\(([^;{}()]*)\)\s*;",
        source,
    )
    if not match:
        errors.append(f"{relative}: callback {name} not found")
        continue
    checked += 1
    prior = text.rfind("*/", 0, match.start())
    first = text.rfind("/**", 0, prior + 1)
    if first < 0 or prior < 0 or ";" in source[prior + 2:match.start()]:
        errors.append(f"{relative}: callback {name}: missing Doxygen")
        continue
    doc = text[first:prior + 2]
    for parameter in parameters(match.group(1)):
        if not re.search(r"@param\s+" + re.escape(parameter) + r"\b", doc):
            errors.append(f"{relative}: callback {name}: missing @param {parameter}")

if errors:
    print("\n".join(errors), file=sys.stderr)
    raise SystemExit(1)
print(f"Checked Doxygen for {checked} public DNS API declarations.")
