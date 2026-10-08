// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QtCrypto>
#include <cstdlib>
#include <iris/jingle-session.h>
#include <iris/jingle.h>
#include <iris/xmpp.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_features.h>

static void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    XMPP::Client     client;
    client.start(QStringLiteral("example.test"), QStringLiteral("owner"), QString(), QStringLiteral("desktop"));
    auto *caps = client.capsManager();
    caps->setEnabled(false); // Deterministic disco completion below; no network.
    XMPP::DiscoItem item;
    XMPP::Features  features;
    const QString   transport = QStringLiteral("urn:xmpp:jingle:transports:ibb:1");
    features.addFeature(transport);
    item.setFeatures(features);
    XMPP::CapsSpec  spec(QStringLiteral("https://iris.test/own-resource"), QCryptographicHash::Sha256,
                         item.capsHash(QCryptographicHash::Sha256));
    const XMPP::Jid peer(QStringLiteral("owner@example.test/phone"));
    caps->updateCaps(client.jid(), spec);
    check(!caps->capsEnabled(client.jid()), "Our exact resource must not query itself");
    caps->updateCaps(peer, spec);
    check(caps->capsEnabled(peer), "Another resource of our account lost its capabilities");
    XMPP::DiscoItem forged;
    caps->updateDisco(peer, forged);
    check(!caps->features(peer).test(transport), "Unverified disco poisoned the capabilities cache");
    caps->updateDisco(peer, item);
    check(caps->features(peer).test(transport), "Verified own-resource disco was not cached");
    XMPP::Jingle::Session session(client.jingleManager(), peer, XMPP::Jingle::Origin::Initiator);
    check(session.checkPeerCaps(transport), "Jingle cannot select a transport for another own resource");
    caps->disableCaps(peer);
    check(!session.checkPeerCaps(transport), "Unavailable peer retained stale transport capabilities");
    return 0;
}
