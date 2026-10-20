// SPDX-License-Identifier: LGPL-2.1-or-later
#include "fixture.h"
#include <iris/xmpp_blocking.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

static QDomDocument parse(const QString &xml)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "Invalid blocking XML test fixture");
    return doc;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint endpoint;
    BlockingTask blocklist(endpoint.client.rootTask());
    blocklist.get();

    auto reply = parse(QStringLiteral(
        "<iq type='result' id='%1'><blocklist xmlns='urn:xmpp:blocking'>"
        "<item jid='peer@example.test'/><item jid='resource@example.test/phone'/>"
        "</blocklist></iq>").arg(blocklist.id()));
    check(blocklist.take(reply.documentElement()), "Blocklist IQ result wasn't consumed");
    check(blocklist.success() && blocklist.blockedJids().size() == 2,
          "Blocklist was not decoded");
    check(blocklist.blockedJids().at(1).resource() == QStringLiteral("phone"),
          "Full-resource block target lost");

    BlockingPushTask listener(endpoint.client.rootTask());
    int notifications = 0;
    QList<Jid> received;
    QObject::connect(&listener, &BlockingPushTask::pushed, &app,
                     [&](const QList<Jid> &jids, bool blocked, bool all) {
        ++notifications;
        received = jids;
        check(blocked && !all, "Unexpected blocking push state");
    });
    auto badSender = parse(QStringLiteral(
        "<iq type='set' from='attacker@example.net' id='push1'>"
        "<block xmlns='urn:xmpp:blocking'><item jid='victim@example.test'/></block></iq>"));
    check(!listener.take(badSender.documentElement()) && notifications == 0,
          "Untrusted XEP-0191 push accepted");
    auto forgedResource = parse(QStringLiteral(
        "<iq type='set' from='alice@example.test/other-device' id='push-other'>"
        "<block xmlns='urn:xmpp:blocking'><item jid='victim@example.test'/></block></iq>"));
    check(!listener.take(forgedResource.documentElement()) && notifications == 0,
          "XEP-0191 resource-originated spoofing accepted");
    auto badNamespace = parse(QStringLiteral(
        "<iq type='set' from='example.test' id='push2'>"
        "<block xmlns='urn:not:blocking'><item jid='victim@example.test'/></block></iq>"));
    check(!listener.take(badNamespace.documentElement()), "Wrong blocking namespace accepted");
    auto good = parse(QStringLiteral(
        "<iq type='set' from='example.test' id='push3'>"
        "<block xmlns='urn:xmpp:blocking'><item jid='victim@example.test'/></block></iq>"));
    check(listener.take(good.documentElement()) && notifications == 1,
          "Valid XEP-0191 push lost");
    check(received.size() == 1 && received.first().bare() == QStringLiteral("victim@example.test"),
          "XEP-0191 pushed JID incorrect");
    return 0;
}
