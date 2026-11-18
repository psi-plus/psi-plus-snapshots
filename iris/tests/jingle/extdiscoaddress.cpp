// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../../src/xmpp/xmpp-im/stunserviceaddress_p.h"
#include <iris/xmpp-im/xmpp_externalservicediscovery.h>

#include <QCoreApplication>
#include <QDomDocument>

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static bool parseService(const QString &host)
{
    QDomDocument document;
    auto element = document.createElement(QStringLiteral("service"));
    element.setAttribute(QStringLiteral("host"), host);
    element.setAttribute(QStringLiteral("type"), QStringLiteral("stun"));
    element.setAttribute(QStringLiteral("port"), QStringLiteral("3478"));
    XMPP::ExternalService service;
    return service.parse(element, false, false);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using XMPP::isUnusableStunServiceLiteral;
    using XMPP::isUsableStunServiceAddress;

    check(isUnusableStunServiceLiteral(QStringLiteral("0.0.0.0")), "IPv4 unspecified literal accepted");
    check(isUnusableStunServiceLiteral(QStringLiteral("::")), "IPv6 unspecified literal accepted");
    check(!isUnusableStunServiceLiteral(QStringLiteral("turn.example.test")), "DNS name rejected");
    check(isUsableStunServiceAddress(QHostAddress(QStringLiteral("192.0.2.10"))), "IPv4 address rejected");
    check(isUsableStunServiceAddress(QHostAddress(QStringLiteral("2001:db8::10"))), "IPv6 address rejected");
    check(!isUsableStunServiceAddress(QHostAddress(QStringLiteral("0.0.0.0"))),
          "IPv4 unspecified address accepted");
    check(!isUsableStunServiceAddress(QHostAddress(QStringLiteral("::"))),
          "IPv6 unspecified address accepted");

    check(!parseService(QStringLiteral("0.0.0.0")), "XEP-0215 accepted 0.0.0.0");
    check(!parseService(QStringLiteral("::")), "XEP-0215 accepted ::");
    check(parseService(QStringLiteral("turn.example.test")), "XEP-0215 rejected a hostname");
    check(parseService(QStringLiteral("192.0.2.10")), "XEP-0215 rejected IPv4 server");
    check(parseService(QStringLiteral("2001:db8::10")), "XEP-0215 rejected IPv6 server");
    return 0;
}
