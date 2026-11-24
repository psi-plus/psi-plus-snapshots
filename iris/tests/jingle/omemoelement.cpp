// SPDX-License-Identifier: LGPL-2.1-or-later
// OMEMO element encryption tests for Jingle and other XMPP extension payloads.
#include "../omemo/fixture.h"

namespace {
void exercise(Endpoint &alice, Endpoint &bob, OmemoProtocol protocol, const char *name)
{
    QDomDocument document;
    const auto   content = fingerprint(document);

    // A fingerprint for one device of the peer, as for OMEMO-verified DTLS.
    auto encrypt
        = wait(alice.omemo->encryptElement(bob.jid, protocol, content, { bob.omemo->ownDeviceId() }), alice, bob);
    check(encrypt->success(), "the element was not encrypted");
    const auto encrypted = encrypt->stanza();
    check(keyCount(encrypted) == 1, "the element was not encrypted for exactly the chosen device");

    auto decrypt = wait(bob.omemo->decryptElement(alice.jid, encrypted), alice, bob);
    check(decrypt->success(), "the element was not decrypted");
    check(decrypt->metadata().senderDeviceId == alice.omemo->ownDeviceId() && !decrypt->metadata().senderKey.isEmpty()
              && decrypt->metadata().details.contains(QStringLiteral("trustLevel")),
          "the decryption does not name the sending device");
    const auto plain = decrypt->stanza();
    if (protocol == OmemoProtocol::Legacy) {
        check(plain.tagName() == QLatin1String("payload") && plain.text() == content.text(),
              "the legacy payload differs");
    } else {
        check(plain.namespaceURI() == DtlsNs && plain.attribute(QStringLiteral("hash")) == QLatin1String("sha-256")
                  && plain.attribute(QStringLiteral("setup")) == QLatin1String("actpass")
                  && plain.text() == content.text(),
              "the OMEMO 2 content differs");
    }

    // The answer goes back to the device that sent the element.
    auto answer = wait(bob.omemo->encryptElement(alice.jid, protocol, content, { decrypt->metadata().senderDeviceId }),
                       alice, bob);
    check(answer->success(), "the answer was not encrypted");
    auto answered = wait(alice.omemo->decryptElement(bob.jid, answer->stanza()), alice, bob);
    check(answered->success(), "the answer was not decrypted");

    // A forged sender cannot be decrypted as someone else's element.
    auto forged
        = wait(bob.omemo->decryptElement(Jid(QStringLiteral("mallory@example.test/device")), encrypted), alice, bob);
    check(!forged->success(), "an element was accepted from the wrong sender");

    if (protocol == OmemoProtocol::Legacy) {
        // A key transport message, as the envelope of JET-OMEMO.
        auto transport = wait(alice.omemo->encryptElement(bob.jid, protocol, QDomElement()), alice, bob);
        check(transport->success(), "the key transport message was not made");
        const auto key = transport->metadata().details.value(QStringLiteral("transportKey")).toByteArray();
        const auto iv  = transport->metadata().details.value(QStringLiteral("transportIv")).toByteArray();
        check(key.size() == 16 && iv.size() == 12, "the key transport message returned no key");
        check(transport->stanza().firstChildElement(QStringLiteral("payload")).isNull(),
              "the key transport message has a payload");
        auto received = wait(bob.omemo->decryptElement(alice.jid, transport->stanza()), alice, bob);
        check(received->success() && received->stanza().isNull()
                  && received->metadata().details.value(QStringLiteral("transportKey")).toByteArray() == key
                  && received->metadata().details.value(QStringLiteral("transportIv")).toByteArray() == iv,
              "the transported key differs");
    } else {
        auto empty = wait(alice.omemo->encryptElement(bob.jid, protocol, QDomElement()), alice, bob);
        check(!empty->success(), "OMEMO 2 accepted an element without content");
    }
    qInfo("OMEMO %s element encryption passed", name);
}

// A MUC service rewrites the outer from/to addresses. The OMEMO 2 SCE
// envelope must still authenticate the *real* sender and the *room*.
// Unknown occupant mappings must never be interpreted as an OMEMO identity.
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        auto setUp = wait(endpoint->omemo->setUp(), alice, bob);
        check(setUp->success() && endpoint->omemo->isReady(), "OMEMO was not set up");
    }

    const auto protocols = alice.omemo->supportedProtocols();
    if (!protocols.testFlag(OmemoProtocol::Legacy) && !protocols.testFlag(OmemoProtocol::Omemo2)) {
        qInfo("OMEMO element tests skipped: QCA offers no usable OMEMO profile");
        pep.clear();
        return 0;
    }
    if (protocols.testFlag(OmemoProtocol::Legacy))
        exercise(alice, bob, OmemoProtocol::Legacy, "legacy");
    if (protocols.testFlag(OmemoProtocol::Omemo2))
        exercise(alice, bob, OmemoProtocol::Omemo2, "2");
    qInfo("OMEMO element tests passed");
    pep.clear(); // the elements must not outlive their document at static destruction
    return 0;
}
