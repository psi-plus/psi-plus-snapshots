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

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("OMEMO bundle item tests passed");
    pep.clear();
    return 0;
}
