// SPDX-License-Identifier: LGPL-2.1-or-later
// Real OMEMO ciphertext through the MAM task and archive replay session.
#include "../omemo/fixture.h"
#include <iris/xmpp_mamtask.h>
#include <iris/xmpp_xmlcommon.h>

namespace {
QDomDocument encrypt(Endpoint &sender, Endpoint &recipient, const QString &profile,
                     const QString &id, const QString &body)
{
    QDomDocument doc;
    auto message = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("message"));
    doc.appendChild(message);
    message.setAttribute(QStringLiteral("type"), QStringLiteral("chat"));
    message.setAttribute(QStringLiteral("from"), sender.jid.full());
    message.setAttribute(QStringLiteral("to"), recipient.jid.full());
    message.setAttribute(QStringLiteral("id"), id);
    auto text = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("body"));
    text.appendChild(doc.createTextNode(body));
    message.appendChild(text);

    EncryptionContext context;
    context.recipients = { recipient.jid.bare() };
    context.options.insert(QStringLiteral("omemoProtocol"), profile);
    auto *session = sender.client.encryptionManager()->startSession(
        OmemoEncryption::methodId(), EncryptionMethod::XmppStanza, context);
    check(session != nullptr, "Cannot create OMEMO sender session");
    auto *job = wait(session->encrypt(message), sender, recipient);
    check(job->success(), "Cannot encrypt archived private message");
    QDomDocument encrypted;
    encrypted.appendChild(encrypted.importNode(job->stanza(), true));
    check(encrypted.documentElement().firstChildElement(QStringLiteral("body")).isNull()
              || encrypted.documentElement().firstChildElement(QStringLiteral("body")).text() != body,
          "Archive contains plaintext");
    session->close();
    return encrypted;
}

void exercise(const QString &profile)
{
    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        auto *setup = wait(endpoint->omemo->setUp(), alice, bob);
        check(setup->success(), "OMEMO setup failed");
    }
    // The sender encrypts while the recipient is offline. No live delivery
    // consumes the prekey message before the archive is fetched.
    const auto first = encrypt(alice, bob, profile, QStringLiteral("first"), QStringLiteral("first archived text"));
    const auto second = encrypt(alice, bob, profile, QStringLiteral("second"), QStringLiteral("second archived text"));
    pump(alice, bob);
    bob.stream.written.clear();

    MAMTask mam(bob.client.rootTask());
    mam.get(alice.jid, QDateTime(), QDateTime(), false, 10, 0, true, true);
    mam.go();
    check(!bob.stream.written.isEmpty(), "Missing MAM query");
    const auto iq = bob.stream.written.takeLast();
    const auto queryId = iq.firstChildElement(QStringLiteral("query")).attribute(QStringLiteral("queryid"));
    check(!queryId.isEmpty(), "MAM query has no queryid");

    const QList<QDomDocument> sent { first, second };
    for (int i = 0; i < sent.size(); ++i) {
        QDomDocument envelope;
        auto message = envelope.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("message"));
        envelope.appendChild(message);
        message.setAttribute(QStringLiteral("from"), bob.jid.bare());
        auto result = envelope.createElementNS(QStringLiteral("urn:xmpp:mam:2"), QStringLiteral("result"));
        result.setAttribute(QStringLiteral("queryid"), queryId);
        result.setAttribute(QStringLiteral("id"), QString::number(i + 1));
        message.appendChild(result);
        auto forwarded = envelope.createElementNS(QStringLiteral("urn:xmpp:forward:0"), QStringLiteral("forwarded"));
        result.appendChild(forwarded);
        auto delay = envelope.createElementNS(QStringLiteral("urn:xmpp:delay"), QStringLiteral("delay"));
        delay.setAttribute(QStringLiteral("stamp"), QStringLiteral("2026-10-18T10:00:0%1Z").arg(i));
        forwarded.appendChild(delay);
        forwarded.appendChild(envelope.importNode(sent.at(i).documentElement(), true));
        QDomDocument parsed;
        check(XMLHelper::parseDocument(parsed, envelope.toByteArray(-1)), "Cannot parse MAM result");
        // MAM buffers the result in its own document.
        check(bob.client.rootTask()->take(parsed.documentElement()), "MAM did not accept archived OMEMO message");
    }
    QDomDocument fin;
    check(XMLHelper::parseDocument(fin, QStringLiteral(
        "<iq from='%1' type='result' id='%2'><fin xmlns='urn:xmpp:mam:2' complete='true'>"
        "<set xmlns='http://jabber.org/protocol/rsm'><first>1</first><last>2</last></set></fin></iq>")
        .arg(bob.jid.bare(), iq.attribute(QStringLiteral("id")))), "Cannot parse MAM fin");
    check(bob.client.rootTask()->take(fin.documentElement()), "MAM did not accept fin");
    check(mam.success() && mam.archive().size() == 2, "MAM archive is incomplete");

    EncryptionContext base;
    base.recipients = { alice.jid.bare() };
    auto *session = bob.client.encryptionManager()->startSession(
        OmemoEncryption::methodId(), EncryptionMethod::XmppStanza, base);
    check(session != nullptr, "Cannot create archive OMEMO session");
    EncryptionContext replay;
    replay.setArchiveReplay();
    int acknowledgements = 0;
    onStanza = [&](const Endpoint &source, QDomElement &stanza) {
        if (source.jid == bob.jid && stanza.tagName() == QLatin1String("message")
            && !stanza.firstChildElement(QStringLiteral("encrypted")).isNull())
            ++acknowledgements;
    };
    const QStringList expected { QStringLiteral("first archived text"), QStringLiteral("second archived text") };
    for (int i = 0; i < mam.archive().size(); ++i) {
        const auto stanza = mam.archive().at(i).firstChildElement(QStringLiteral("forwarded"))
                                .firstChildElement(QStringLiteral("message"));
        auto *job = wait(session->decrypt(stanza, replay), alice, bob);
        check(job->success(), "Real archived OMEMO message did not decrypt");
        check(job->stanza().firstChildElement(QStringLiteral("body")).text() == expected.at(i),
              "Archived OMEMO plaintext mismatch");
        check(job->metadata().sender.bare() == alice.jid.bare(), "Archived sender identity mismatch");
    }
    pump(alice, bob);
    check(acknowledgements == 0, "Archive replay sent an OMEMO acknowledgement");
    session->close();
    onStanza = {};
    // A sending device has no recipient key in its own archived ciphertext.
    // The application must use its local history, not a second decryption.
    EncryptionContext own;
    own.recipients = { bob.jid.bare() };
    auto *ownSession = alice.client.encryptionManager()->startSession(
        OmemoEncryption::methodId(), EncryptionMethod::XmppStanza, own);
    check(ownSession != nullptr, "Cannot create own archive session");
    auto *ownJob = wait(ownSession->decrypt(first.documentElement(), replay), alice, bob);
    check(!ownJob->success() && ownJob->error() == EncryptionJob::Error::NoRecipients,
          "Sending device unexpectedly decrypted its own ciphertext");
    ownSession->close();
    pep.clear();
    qInfo("Private MAM OMEMO %s replay passed", qPrintable(profile));
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    OmemoProtocols supported;
    {
        Endpoint probe(QStringLiteral("probe"));
        supported = probe.omemo->supportedProtocols();
    }
    if (supported.testFlag(OmemoProtocol::Omemo2))
        exercise(QStringLiteral("omemo2"));
    if (supported.testFlag(OmemoProtocol::Legacy))
        exercise(QStringLiteral("legacy"));
    return 0;
}
