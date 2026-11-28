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
        auto *session = alice.client.encryptionManager()->startSession(OmemoEncryption::methodId(),
                                                                       EncryptionMethod::XmppStanza | EncryptionMethod::Groupchat, send);
        check(session != nullptr, "Cannot create OMEMO sender session");
        auto *job = session->encrypt(stanza);
        session->close();
        wait(job, alice, bob);
        check(job->success(), "MUC OMEMO 2 encryption failed");
        check(job->metadata().details.value(QStringLiteral("omemoProtocol")).toString() == QStringLiteral("omemo2"),
              "Groupchat encryption did not select OMEMO 2 automatically");

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

    // The room can return history from a sender who is no longer present.
    // Use the historical muc#user identity, not the current nick mapping.
    check(bob.client.groupChatJoin(room.domain(), room.node(), QStringLiteral("Bob")), "Could not join test MUC");
    QDomDocument presenceDoc;
    auto presence = presenceDoc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("presence"));
    presenceDoc.appendChild(presence);
    presence.setAttribute(QStringLiteral("from"), room.bare() + QStringLiteral("/Bob"));
    auto presenceUser = presenceDoc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"),
                                                     QStringLiteral("x"));
    auto selfItem = presenceDoc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"),
                                                 QStringLiteral("item"));
    selfItem.setAttribute(QStringLiteral("jid"), bob.jid.full());
    selfItem.setAttribute(QStringLiteral("affiliation"), QStringLiteral("member"));
    selfItem.setAttribute(QStringLiteral("role"), QStringLiteral("participant"));
    presenceUser.appendChild(selfItem);
    auto selfStatus = presenceDoc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"),
                                                   QStringLiteral("status"));
    selfStatus.setAttribute(QStringLiteral("code"), QStringLiteral("110"));
    presenceUser.appendChild(selfStatus);
    presence.appendChild(presenceUser);
    bob.stream.inject(presence);

    auto delayedDoc = encryptMessage();
    auto delayed = delayedDoc.documentElement();
    auto historyUser = delayedDoc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"),
                                                   QStringLiteral("x"));
    auto historyItem = delayedDoc.createElementNS(QStringLiteral("http://jabber.org/protocol/muc#user"),
                                                   QStringLiteral("item"));
    historyItem.setAttribute(QStringLiteral("jid"), alice.jid.bare());
    historyUser.appendChild(historyItem);
    delayed.appendChild(historyUser);
    auto delay = delayedDoc.createElementNS(QStringLiteral("urn:xmpp:delay"), QStringLiteral("delay"));
    delay.setAttribute(QStringLiteral("from"), room.bare());
    delay.setAttribute(QStringLiteral("stamp"), QStringLiteral("2026-01-01T00:00:00Z"));
    delayed.appendChild(delay);

    QObject observation;
    bool receivedHistory = false;
    QObject::connect(&bob.client, &Client::messageReceived, &observation, [&](const Message &message) {
        const auto *metadata = bob.client.currentEncryptionMetadata();
        if (message.type() == Message::Type::Groupchat && message.from() == occupant) {
            receivedHistory = message.wasEncrypted() && message.body() == QStringLiteral("Private MUC message")
                && metadata && metadata->details.value(QStringLiteral("archiveReplay")).toBool();
        }
    });
    bob.stream.inject(delayed);
    for (int i = 0; i < 30 && !receivedHistory; ++i)
        pump(alice, bob);
    check(receivedHistory, "Room-delayed OMEMO history did not use the historical sender/replay context");

    // A historical message not encrypted for this device must retain its
    // sender, time and ID, but must not expose an unauthenticated fallback body.
    auto missingKeyDoc = encryptMessage();
    auto missingKey = missingKeyDoc.documentElement();
    missingKey.setAttribute(QStringLiteral("id"), QStringLiteral("unavailable-history"));
    missingKey.appendChild(missingKeyDoc.importNode(historyUser, true));
    missingKey.appendChild(missingKeyDoc.importNode(delay, true));
    auto header = missingKey.firstChildElement(QStringLiteral("encrypted"))
                          .firstChildElement(QStringLiteral("header"));
    for (auto keys = header.firstChildElement(QStringLiteral("keys")); !keys.isNull();
         keys = header.firstChildElement(QStringLiteral("keys")))
        header.removeChild(keys);
    auto fallback = missingKeyDoc.createElement(QStringLiteral("body"));
    fallback.appendChild(missingKeyDoc.createTextNode(QStringLiteral("untrusted fallback")));
    missingKey.appendChild(fallback);
    bool receivedFailure = false;
    QObject::connect(&bob.client, &Client::messageReceived, &observation, [&](const Message &message) {
        if (message.id() == QStringLiteral("unavailable-history"))
            receivedFailure = message.from() == occupant && message.spooled() && message.wasEncrypted()
                && message.body().isEmpty()
                && message.decryptionFailure() == Message::DecryptionFailure::NotForThisDevice;
    });
    bob.stream.inject(missingKey);
    for (int i = 0; i < 30 && !receivedFailure; ++i)
        pump(alice, bob);
    check(receivedFailure, "Failed MUC history was not delivered with a safe local failure status");

    // Real servers can omit the historical muc#user element from join
    // history. The host must still get a chance to restore cached plaintext.
    // This fixture has no history cache, so actual decryption must fail.
    auto noIdentityDoc = encryptMessage();
    auto noIdentity = noIdentityDoc.documentElement();
    noIdentity.setAttribute(QStringLiteral("id"), QStringLiteral("history-without-real-jid"));
    noIdentity.appendChild(noIdentityDoc.importNode(delay, true));
    bool missingIdentityFailed = false;
    QObject::connect(&bob.client, &Client::messageReceived, &observation, [&](const Message &message) {
        if (message.id() == QStringLiteral("history-without-real-jid"))
            missingIdentityFailed = message.spooled() && message.body().isEmpty()
                && message.decryptionFailure() == Message::DecryptionFailure::Failed;
    });
    const int handledBeforeReplay = bob.incomingHandled;
    bob.stream.inject(noIdentity);
    for (int i = 0; i < 30 && !missingIdentityFailed; ++i)
        pump(alice, bob);
    check(bob.incomingHandled == handledBeforeReplay + 1,
          "Missing historical JID prevented the host's history lookup");
    check(missingIdentityFailed, "Historical decryption without a real sender must fail closed");

    qInfo("OMEMO 2 authenticated MUC sender, room binding and join history passed");
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
    QString receivedArchiveId;
    bool wasEncrypted = false;
    QObject::connect(&alice.client, &Client::messageReceived, &alice.client, [&](const Message &m) {
        if (m.type() == Message::Type::Groupchat) {
            ++ownMessages;
            receivedBody = m.body();
            receivedArchiveId = m.stanzaId(room).id;
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
    check(ownMessages == 0, "Local send completed before the room confirmed delivery");

    QDomDocument echoed;
    auto stanza = echoed.importNode(captured.documentElement(), true).toElement();
    echoed.appendChild(stanza);
    stanza.setAttribute(QStringLiteral("from"), selfOccupant);
    stanza.setAttribute(QStringLiteral("to"), alice.jid.full());
    auto archiveId = echoed.createElementNS(QStringLiteral("urn:xmpp:sid:0"), QStringLiteral("stanza-id"));
    archiveId.setAttribute(QStringLiteral("by"), room.bare());
    archiveId.setAttribute(QStringLiteral("id"), QStringLiteral("room-archive-id"));
    stanza.appendChild(archiveId);

    // A different occupant with the same ID must not consume the echo.
    QDomDocument foreign;
    auto other = foreign.importNode(stanza, true).toElement();
    foreign.appendChild(other);
    other.setAttribute(QStringLiteral("from"), room.bare() + QStringLiteral("/Other"));
    alice.stream.inject(other);
    check(ownMessages == 0 && decryptionFailures == 1, "Another occupant consumed our pending echo");

    // An unknown ID from our own occupant is not a pending echo either.
    QDomDocument unknown;
    auto unknownId = unknown.importNode(stanza, true).toElement();
    unknown.appendChild(unknownId);
    unknownId.setAttribute(QStringLiteral("id"), QStringLiteral("unknown-id"));
    alice.stream.inject(unknownId);
    pump(alice, bob);
    check(ownMessages == 0, "Unknown message ID consumed our pending echo");
    const int failuresBeforeEcho = decryptionFailures;

    // Reconcile the locally generated plaintext without comparing ciphertext.
    auto payload = stanza.firstChildElement(QStringLiteral("encrypted"))
                       .firstChildElement(QStringLiteral("payload"));
    check(!payload.isNull(), "Missing encrypted payload");
    payload.appendChild(echoed.createTextNode(QStringLiteral("changed")));
    alice.stream.inject(stanza);
    check(ownMessages == 1 && receivedBody == QStringLiteral("A protected groupchat message") && wasEncrypted
              && receivedArchiveId == QStringLiteral("room-archive-id")
              && decryptionFailures == failuresBeforeEcho,
          "The outgoing OMEMO MUC echo was not delivered with the room's archive ID");

    alice.stream.inject(stanza);
    check(ownMessages == 1 && decryptionFailures == failuresBeforeEcho,
          "A duplicate OMEMO MUC self echo was shown or reported as a decryption error");
    onStanza = {};
    qInfo("OMEMO 2 MUC self echo reconciliation passed");
}
// A first send to multiple unknown recipients must discover all identities
// before the application asks for trust, rather than failing one at a time.
void exerciseGroupchatTrustDiscovery()
{
    Endpoint sender(QStringLiteral("trust-sender"));
    Endpoint first(QStringLiteral("trust-first"));
    Endpoint second(QStringLiteral("trust-second"));
    for (auto *endpoint : { &sender, &first, &second }) {
        auto *job = wait(endpoint->omemo->setUp(), *endpoint, sender);
        check(job->success(), "OMEMO setup for trust discovery failed");
    }
    sender.omemo->setNewIdentityTrustLevel(EncryptionTrustLevel::Undecided);
    sender.omemo->setAcceptedSessionBuildingTrustLevels(EncryptionTrustLevel::ManuallyTrusted);
    const Jid room(QStringLiteral("trust@conference.example.test"));
    Message message(room);
    message.setType(Message::Type::Groupchat);
    message.setBody(QStringLiteral("Trust discovery"));
    EncryptionContext context;
    context.recipients = { first.jid.bare(), second.jid.bare() };
    auto *session = sender.client.encryptionManager()->startSession(
        OmemoEncryption::methodId(), EncryptionMethod::XmppStanza | EncryptionMethod::Groupchat, context);
    check(session != nullptr, "Cannot create groupchat trust test session");
    auto *job = wait(session->encrypt(message.toStanza(&sender.client.stream()).element()), sender, first);
    check(!job->success() && job->error() == EncryptionJob::Error::UntrustedIdentity,
          "Unknown recipients must prevent encryption");
    for (const auto *recipient : { &first, &second }) {
        bool discovered = false;
        for (const auto &device : sender.omemo->devices(recipient->jid)) {
            if (device.protocol != OmemoProtocol::Omemo2 || device.identityKey.isEmpty())
                continue;
            discovered = true;
            check(device.trust == EncryptionTrustLevel::Undecided, "Discovery silently trusted a device");
            check(sender.omemo->setTrustLevel(device.owner, device.identityKey, EncryptionTrustLevel::ManuallyTrusted),
                  "Could not approve discovered identity");
        }
        check(discovered, "First encryption attempt did not discover every recipient identity");
    }
    job = wait(session->encrypt(message.toStanza(&sender.client.stream()).element()), sender, first);
    check(job->success(), "Groupchat encryption failed after approving all discovered identities");
    session->close();
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
    const bool modern = alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Omemo2);
    check(alice.omemo->capabilities().testFlag(EncryptionMethod::Groupchat) == modern,
          "Groupchat capability must depend on OMEMO 2 support");
    if (modern) {
        check(alice.omemo->groupchatRequirements() == (EncryptionMethod::MembersOnly | EncryptionMethod::NonAnonymous
                                                       | EncryptionMethod::MemberJids),
              "Incorrect OMEMO groupchat requirements");
        exerciseHostOwnedIncomingSessions(alice, bob);
        exerciseMuc(alice, bob);
        exerciseMucSelfEcho(alice, bob);
        exerciseGroupchatTrustDiscovery();
    } else {
        qInfo("OMEMO MUC tests skipped: OMEMO 2 unavailable");
    }
    pep.clear();
    return 0;
}
