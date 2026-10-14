// SPDX-License-Identifier: LGPL-2.1-or-later
// OMEMO 2 keeps the bundles of all devices of an account in one PEP node. A bundle fetched for one device
// must be that device's item: a server that answers with another item must not get another device's identity
// pinned. Identities pinned that way by earlier builds are forgotten at start. Reuses the OMEMO fixture.
#define main iris_omemoelement_main
#include "omemoelement.cpp"
#undef main

namespace {
QByteArray pinned(const Endpoint &endpoint, const Jid &owner, uint32_t id)
{
    return endpoint.storage.allData()
        .devices.value(owner.bare())
        .value(id)
        .protocols.value(OmemoProtocol::Omemo2)
        .keyId;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    Endpoint carol(QStringLiteral("carol"));
    for (auto endpoint : { &alice, &bob, &carol }) {
        auto setUp = wait(endpoint->omemo->setUp(), *endpoint, *endpoint);
        check(setUp->success(), "OMEMO was not set up");
    }
    if (!alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Omemo2)) {
        qInfo("OMEMO bundle item tests skipped: no OMEMO 2");
        pep.clear();
        return 0;
    }
    const auto bobId   = bob.omemo->ownDeviceId();
    const auto bundles = OmemoEncryption::bundlesNode();
    const auto bobJid  = bob.jid.bare();

    // Bob's bundles node holds only the bundle of his other device (carol's keys stand in for it), and the
    // server answers with it whatever item is asked for.
    const auto bobBundle = pep[bobJid][bundles].value(QString::number(bobId));
    check(!bobBundle.isNull(), "bob's OMEMO 2 bundle was not published");
    pep[bobJid][bundles].clear();
    pep[bobJid][bundles][QString::number(carol.omemo->ownDeviceId())]
        = pep[carol.jid.bare()][bundles].value(QString::number(carol.omemo->ownDeviceId()));
    pepIgnoresItemIds = true;
    QDomDocument document;
    const auto   content = fingerprint(document);
    auto wrong = wait(alice.omemo->encryptElement(bob.jid, OmemoProtocol::Omemo2, content, { bobId }), alice, bob);
    check(!wrong->success(), "an element was encrypted with another device's bundle");
    check(pinned(alice, bob.jid, bobId).isEmpty(), "another device's identity was pinned");

    // With his own item the encryption works and bob can read it.
    pep[bobJid][bundles][QString::number(bobId)] = bobBundle;
    auto right = wait(alice.omemo->encryptElement(bob.jid, OmemoProtocol::Omemo2, content, { bobId }), alice, bob);
    check(right->success(), "the element was not encrypted with the device's own bundle");
    auto read = wait(bob.omemo->decryptElement(alice.jid, right->stanza()), alice, bob);
    check(read->success(), "the element encrypted with the right bundle was not decrypted");
    pepIgnoresItemIds = false;

    // An identity pinned for two devices of one account by an earlier build is forgotten at start, with its
    // sessions, and learnt again.
    const auto key       = pinned(alice, bob.jid, bobId);
    const auto otherId   = carol.omemo->ownDeviceId();
    auto       data      = alice.storage.allData();
    auto       duplicate = data.devices.value(bobJid).value(bobId);
    check(!key.isEmpty() && !duplicate.protocols.value(OmemoProtocol::Omemo2).session.isEmpty(),
          "bob's identity and session were not stored");
    delete alice.omemo;
    alice.storage.addDevice(bobJid, otherId, duplicate);
    alice.omemo = new OmemoEncryption(&alice.client, &alice.storage, &alice.trust);
    check(alice.omemo->isReady(), "OMEMO did not restart");
    check(pinned(alice, bob.jid, bobId).isEmpty() && pinned(alice, bob.jid, otherId).isEmpty()
              && alice.storage.allData()
                     .devices.value(bobJid)
                     .value(bobId)
                     .protocols.value(OmemoProtocol::Omemo2)
                     .session.isEmpty(),
          "an identity pinned for two devices was kept");
    auto again = wait(alice.omemo->encryptElement(bob.jid, OmemoProtocol::Omemo2, content, { bobId }), alice, bob);
    check(again->success() && pinned(alice, bob.jid, bobId) == key, "the forgotten identity was not learnt again");
    auto readAgain = wait(bob.omemo->decryptElement(alice.jid, again->stanza()), alice, bob);
    check(readAgain->success(), "the element after relearning was not decrypted");

    // Sanitizing the own PEP data retires an own device that has no bundle at all, and keeps one whose OMEMO 2
    // bundle a one-item node lost but whose legacy bundle (a node per device) is there.
    if (alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Legacy)) {
        const auto     aliceJid = alice.jid.bare();
        const uint32_t dead = 111111, legacyOnly = 222222;
        const auto     ownId = alice.omemo->ownDeviceId();
        auto           list  = [](const QString &ns, const QString &root, std::initializer_list<uint32_t> ids) {
            auto element = pepDocument.createElementNS(ns, root);
            for (const auto id : ids) {
                auto device = pepDocument.createElementNS(ns, QStringLiteral("device"));
                device.setAttribute(QStringLiteral("id"), QString::number(id));
                element.appendChild(device);
            }
            return element;
        };
        const auto legacyNs = QStringLiteral("eu.siacs.conversations.axolotl");
        pep[aliceJid][OmemoEncryption::devicesNode()][QStringLiteral("current")]
            = list(OmemoEncryption::namespaceUri(), QStringLiteral("devices"), { ownId, dead, legacyOnly });
        pep[aliceJid][OmemoEncryption::legacyDevicesNode()][QStringLiteral("current")]
            = list(legacyNs, QStringLiteral("list"), { ownId, dead, legacyOnly });
        const auto bobLegacy = pep[bobJid][legacyNs + QStringLiteral(".bundles:") + QString::number(bobId)];
        check(!bobLegacy.isEmpty(), "bob's legacy bundle was not published");
        pep[aliceJid][legacyNs + QStringLiteral(".bundles:") + QString::number(legacyOnly)] = bobLegacy;
        auto refresh = wait(alice.omemo->refreshDevices(alice.jid, OmemoProtocol::Legacy, false), alice, bob);
        check(refresh->success(), "the own legacy device list was not read");

        auto sanitize = wait(alice.omemo->sanitizeOwnPep(), alice, bob);
        check(sanitize->success(), "sanitizing the own PEP data failed");
        const auto listed = [&](const QString &node) {
            QList<uint32_t> ids;
            const auto      element = pep[aliceJid][node].value(QStringLiteral("current"));
            for (auto device = element.firstChildElement(QStringLiteral("device")); !device.isNull();
                 device      = device.nextSiblingElement(QStringLiteral("device")))
                ids.append(device.attribute(QStringLiteral("id")).toUInt());
            return ids;
        };
        for (const auto &node : { OmemoEncryption::devicesNode(), OmemoEncryption::legacyDevicesNode() }) {
            const auto ids = listed(node);
            check(ids.contains(ownId) && ids.contains(legacyOnly) && !ids.contains(dead),
                  "sanitizing did not retire exactly the own device without any bundle");
        }
    }

    // A chat message goes with legacy OMEMO when a device of the recipient has no OMEMO 2 bundle but a legacy one,
    // and every device OMEMO 2 would reach is reachable with legacy OMEMO too. Otherwise it stays OMEMO 2.
    if (alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Legacy)) {
        Endpoint dave(QStringLiteral("dave"));
        Endpoint erin(QStringLiteral("erin"));
        for (auto endpoint : { &dave, &erin }) {
            auto setUp = wait(endpoint->omemo->setUp(), dave, erin);
            check(setUp->success(), "OMEMO was not set up");
        }
        const auto     legacyNs = QStringLiteral("eu.siacs.conversations.axolotl");
        const auto     erinJid  = erin.jid.bare();
        const auto     erinId   = erin.omemo->ownDeviceId();
        const uint32_t second   = 333333;
        auto           list     = [](const QString &ns, const QString &root, std::initializer_list<uint32_t> ids) {
            auto element = pepDocument.createElementNS(ns, root);
            for (const auto id : ids) {
                auto device = pepDocument.createElementNS(ns, QStringLiteral("device"));
                device.setAttribute(QStringLiteral("id"), QString::number(id));
                element.appendChild(device);
            }
            return element;
        };
        const auto send = [&](const QString &to) {
            QDomDocument document;
            auto         message = document.createElement(QStringLiteral("message"));
            message.setAttribute(QStringLiteral("to"), to);
            message.setAttribute(QStringLiteral("type"), QStringLiteral("chat"));
            auto body = document.createElement(QStringLiteral("body"));
            body.appendChild(document.createTextNode(QStringLiteral("hello")));
            message.appendChild(body);
            document.appendChild(message);
            EncryptionContext context;
            context.recipients = { Jid(to) };
            std::unique_ptr<EncryptedSession> session(dave.omemo->startSession(EncryptionMethod::XmppStanza, context));
            auto                              job = wait(session->encrypt(message), dave, erin);
            check(job->success(), "the chat message was not encrypted");
            return job->stanza().firstChildElement(QStringLiteral("encrypted")).namespaceURI();
        };
        check(send(erinJid) == OmemoEncryption::namespaceUri(),
              "a message to devices with OMEMO 2 bundles left OMEMO 2");

        pep[erinJid][OmemoEncryption::devicesNode()][QStringLiteral("current")]
            = list(OmemoEncryption::namespaceUri(), QStringLiteral("devices"), { erinId, second });
        pep[erinJid][OmemoEncryption::legacyDevicesNode()][QStringLiteral("current")]
            = list(legacyNs, QStringLiteral("list"), { erinId, second });
        pep[erinJid][legacyNs + QStringLiteral(".bundles:") + QString::number(second)]
            = pep[carol.jid.bare()]
                 [legacyNs + QStringLiteral(".bundles:") + QString::number(carol.omemo->ownDeviceId())];
        auto refresh       = wait(dave.omemo->refreshDevices(erin.jid, OmemoProtocol::Omemo2, false), dave, erin);
        auto refreshLegacy = wait(dave.omemo->refreshDevices(erin.jid, OmemoProtocol::Legacy, false), dave, erin);
        check(refresh->success() && refreshLegacy->success(), "erin's device lists were not read");
        check(send(erinJid) == legacyNs,
              "a message did not fall back to legacy OMEMO for a device without an OMEMO 2 bundle");
    }

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("OMEMO bundle item tests passed");
    pep.clear();
    return 0;
}
