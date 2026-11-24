// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0384 encrypted MUC identity/SCE and own-message echo regressions.
#include "../omemo/fixture.h"
#include <iris/xmpp_message.h>

namespace {
// A resolved conversation must not round-trip into Psi's contact lookup for
// every subsequent stanza. Psi selects and reuses its contact-owned session;
// Iris does not cache contact sessions or look them up by JID.
void exerciseHostOwnedIncomingSessions(Endpoint &alice, Endpoint &bob)
{
    QStringList received;
    int         failures = 0;
    QObject::connect(&bob.client, &Client::messageReceived, &bob.client, [&](const Message &m) {
        if (m.type() == Message::Type::Chat)
            received.append(m.body());
    });
    QObject::connect(&bob.client, &Client::stanzaDecryptionFailed, &bob.client,
                     [&](const QString &, const Jid &, EncryptionJob::Error, const QString &,
                         const EncryptionMetadata &) { ++failures; });

    const auto encrypted = [&](const QString &body) {
        QDomDocument doc;
        auto         message = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("message"));
        doc.appendChild(message);
        message.setAttribute(QStringLiteral("from"), alice.jid.full());
        message.setAttribute(QStringLiteral("to"), bob.jid.full());
        message.setAttribute(QStringLiteral("type"), QStringLiteral("chat"));
        auto bodyElement = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("body"));
        bodyElement.appendChild(doc.createTextNode(body));
        message.appendChild(bodyElement);
        EncryptionContext context;
        context.recipients = { bob.jid.bare() };
        context.options.insert(QStringLiteral("omemoProtocol"), QStringLiteral("omemo2"));
        auto *session = alice.client.encryptionManager()->startSession(OmemoEncryption::methodId(),
                                                                       EncryptionMethod::XmppStanza, context);
        check(session != nullptr, "Cannot create direct OMEMO sender session");
        auto *job = session->encrypt(message);
        session->close();
        wait(job, alice, bob);
        check(job->success(), "Cannot encrypt direct OMEMO routing test stanza");
        QDomDocument wire;
        wire.appendChild(wire.importNode(job->stanza(), true));
        return wire;
    };

    const auto deliver = [&](const QString &body, int expected) {
        auto wire = encrypted(body);
        bob.stream.inject(wire.documentElement());
        for (int i = 0; i < 30 && received.size() < expected; ++i)
            pump(alice, bob);
        check(received.size() == expected && received.last() == body && failures == 0,
              "Explicit incoming decryption or normal message delivery failed");
    };

    const int initialHandled  = bob.incomingHandled;
    const int initialSessions = bob.incomingSessionsCreated;
    deliver(QStringLiteral("first protected message"), 1);
    check(bob.incomingHandled == initialHandled + 1 && bob.incomingSessionsCreated == initialSessions + 1,
          "First stanza did not create a host-owned session");
    deliver(QStringLiteral("second protected message"), 2);
    check(bob.incomingHandled == initialHandled + 2 && bob.incomingSessionsCreated == initialSessions + 1,
          "The host did not reuse its session for a second stanza");

    for (auto session : std::as_const(bob.conversationSessions))
        if (session)
            session->close();
    bob.conversationSessions.clear();
    deliver(QStringLiteral("third protected message"), 3);
    check(bob.incomingHandled == initialHandled + 3 && bob.incomingSessionsCreated == initialSessions + 2,
          "The host reused a retired session");
    qInfo("OMEMO host-owned incoming session routing passed");
}

void exerciseMuc(Endpoint &alice, Endpoint &bob)
{
    const Jid room(QStringLiteral("garden@conference.example.test"));
    const Jid occupant(room.full() + QStringLiteral("/Alice"));

    const auto encryptMessage = [&]() {
        QDomDocument doc;
        auto stanza = doc.createElement(QStringLiteral("message"));
        stanza.setAttribute(QStringLiteral("type"), QStringLiteral("groupchat"));
        stanza.setAttribute(QStringLiteral("from"), alice.jid.full());
        stanza.setAttribute(QStringLiteral("to"), room.bare());
        doc.appendChild(stanza);
        auto body = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("body"));
        body.appendChild(doc.createTextNode(QStringLiteral("Private MUC message")));
        stanza.appendChild(body);

        EncryptionContext send;
        send.recipients = { bob.jid.bare() };
        send.options.insert(QStringLiteral("omemoProtocol"), QStringLiteral("omemo2"));
        auto *session = alice.client.encryptionManager()->startSession(OmemoEncryption::methodId(),
                                                                       EncryptionMethod::XmppStanza, send);
        check(session != nullptr, "Cannot create OMEMO sender session");
        auto *job = session->encrypt(stanza);
        session->close();
        wait(job, alice, bob);
        check(job->success(), "MUC OMEMO 2 encryption failed");

        QDomDocument incomingDoc;
        auto incoming = incomingDoc.importNode(job->stanza(), true).toElement();
        incomingDoc.appendChild(incoming);
        incoming.setAttribute(QStringLiteral("from"), occupant.full());
        incoming.setAttribute(QStringLiteral("to"), bob.jid.full());
        return incomingDoc;
    };

    QDomDocument incomingDoc = encryptMessage();
    auto incoming = incomingDoc.documentElement();
    const auto   decrypt     = [&](const QDomElement &stanza, const EncryptionContext &context) {
        auto *session = bob.client.encryptionManager()->startSession(OmemoEncryption::methodId(),
                                                                           EncryptionMethod::XmppStanza, context);
        check(session != nullptr, "Cannot create OMEMO receiver session");
        auto *job = session->decrypt(stanza);
        session->close();
        return wait(job, alice, bob);
    };
    auto *unknown = decrypt(incoming, {});
    check(!unknown->success() && unknown->error() == EncryptionJob::Error::AuthenticationFailed,
          "MUC ciphertext without a real sender mapping was accepted");

    EncryptionContext receive;
    receive.options.insert(QStringLiteral("mucRealSender"), alice.jid.bare());
    auto *opened = decrypt(incoming, receive);
    check(opened->success(), "MUC message with authenticated real sender failed");
    check(opened->metadata().sender.bare() == alice.jid.bare()
              && opened->stanza().attribute(QStringLiteral("from")) == occupant.full()
              && opened->stanza().attribute(QStringLiteral("to")) == bob.jid.full()
              && opened->stanza().firstChildElement(QStringLiteral("body")).text()
                     == QStringLiteral("Private MUC message"),
          "MUC identity, original addresses or decrypted body were not preserved");

    auto wrongRoomDoc = encryptMessage();
    auto wrongRoom = wrongRoomDoc.documentElement();
    wrongRoom.setAttribute(QStringLiteral("from"), QStringLiteral("other@conference.example.test/Alice"));
    auto *roomAttack = decrypt(wrongRoom, receive);
    check(!roomAttack->success() && roomAttack->error() == EncryptionJob::Error::AuthenticationFailed,
          "MUC SCE <to/> did not bind ciphertext to the original room");

    qInfo("OMEMO 2 authenticated MUC sender and room binding passed");
}

void exerciseMucSelfEcho(Endpoint &alice, Endpoint &bob)
{
    const Jid room(QStringLiteral("garden@conference.example.test"));
    const QString nick(QStringLiteral("Alice"));
    const QString selfOccupant = room.full() + QLatin1Char('/') + nick;

    check(alice.client.groupChatJoin(room.domain(), room.node(), nick), "Could not join test MUC");

    QDomDocument doc;
    auto presence = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("presence"));
    presence.setAttribute(QStringLiteral("from"), selfOccupant);
    doc.appendChild(presence);
    auto x = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"), QStringLiteral("x"));
    auto item = doc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"), QStringLiteral("item"));
    item.setAttribute(QStringLiteral("jid"), alice.jid.full());
    item.setAttribute(QStringLiteral("affiliation"), QStringLiteral("member"));
    item.setAttribute(QStringLiteral("role"), QStringLiteral("participant"));
    x.appendChild(item);
    presence.appendChild(x);
    alice.stream.inject(presence);

    int ownMessages = 0;
    int decryptionFailures = 0;
    QObject::connect(&alice.client, &Client::stanzaDecryptionFailed, &alice.client,
                     [&](const QString &, const Jid &, EncryptionJob::Error, const QString &,
                         const EncryptionMetadata &) { ++decryptionFailures; });
    QString receivedBody;
    bool wasEncrypted = false;
    QObject::connect(&alice.client, &Client::messageReceived, &alice.client, [&](const Message &m) {
        if (m.type() == Message::Type::Groupchat) {
            ++ownMessages;
            receivedBody = m.body();
            wasEncrypted = m.wasEncrypted();
        }
    });

    QDomDocument captured;
    onStanza = [&](const Endpoint &source, QDomElement &stanza) {
        if (source.jid == alice.jid && stanza.attribute(QStringLiteral("type")) == QLatin1String("groupchat")) {
            captured.appendChild(captured.importNode(stanza, true));
        }
    };

    Message outgoing(room);
    outgoing.setType(Message::Type::Groupchat);
    outgoing.setId(QStringLiteral("muc-echo-test-1"));
    outgoing.setBody(QStringLiteral("A protected groupchat message"));
    EncryptionContext context;
    context.recipients = { bob.jid.bare() };
    context.options.insert(QStringLiteral("omemoProtocol"), QStringLiteral("omemo2"));
    auto *job = wait(alice.client.sendMessageEncrypted(outgoing, OmemoEncryption::methodId(), context), alice, bob);
    check(job->success(), "Could not send a MUC encrypted stanza");
    // A completed encryption job need not have driven the fixture's outgoing
    // stream queue. The onStanza hook runs when pump() drains that queue.
    pump(alice, bob);
    check(!captured.documentElement().isNull(), "The encrypted groupchat stanza was not emitted");

    QDomDocument echoed;
    auto stanza = echoed.importNode(captured.documentElement(), true).toElement();
    echoed.appendChild(stanza);
    stanza.setAttribute(QStringLiteral("from"), selfOccupant);
    stanza.setAttribute(QStringLiteral("to"), alice.jid.full());

    // Matching room, nick and message id alone must not authenticate an
    // echo. The ciphertext and encrypted recipient keys must also match.
    QDomDocument forgedDoc;
    auto forged = forgedDoc.importNode(stanza, true).toElement();
    forgedDoc.appendChild(forged);
    auto encryptedElement = forged.firstChildElement(QStringLiteral("encrypted"));
    check(!encryptedElement.isNull(), "OMEMO encrypted element missing in echo test");
    encryptedElement.setAttribute(QStringLiteral("tampered"), QStringLiteral("yes"));
    alice.stream.inject(forged);
    check(ownMessages == 0 && decryptionFailures == 1,
          "MUC echoed a ciphertext not matching the locally sent message");

    alice.stream.inject(stanza);
    check(ownMessages == 1 && receivedBody == QStringLiteral("A protected groupchat message") && wasEncrypted,
          "The outgoing OMEMO MUC echo was not delivered locally as an encrypted message");

    // A duplicate echo has no remaining local plaintext mapping and must not
    // be shown again; neither may a message with the wrong stanza ID.
    alice.stream.inject(stanza);
    check(ownMessages == 1 && decryptionFailures == 1,
          "A duplicate OMEMO MUC self echo was shown or reported as a decryption error");
    onStanza = {};
    qInfo("OMEMO 2 MUC self echo reconciliation passed");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        auto setUp = wait(endpoint->omemo->setUp(), alice, bob);
        check(setUp->success() && endpoint->omemo->isReady(), "OMEMO setup failed");
    }
    if (alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Omemo2)) {
        exerciseHostOwnedIncomingSessions(alice, bob);
        exerciseMuc(alice, bob);
        exerciseMucSelfEcho(alice, bob);
    } else {
        qInfo("OMEMO MUC tests skipped: OMEMO 2 unavailable");
    }
    pep.clear();
    return 0;
}
