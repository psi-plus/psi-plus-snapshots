// SPDX-License-Identifier: LGPL-2.1-or-later
// JET-OMEMO file transfer (JetOmemo) between two clients over IBB: the file travels as AES-GCM with the
// tag appended, a tampered block fails authentication, and the session policy decides what happens
// without peer support. Reuses the OMEMO fixture with its in-memory PEP service.
#define main iris_omemoelement_main
#include "omemoelement.cpp"
#undef main

#include <iris/jingle-ft.h>
#include <iris/jingle-jet-omemo.h>
#include <iris/jingle-session.h>
#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_hash.h>

#include <QBuffer>
#include <QRandomGenerator>

namespace J  = XMPP::Jingle;
namespace FT = XMPP::Jingle::FileTransfer;

namespace {
const QString IbbNs(QStringLiteral("http://jabber.org/protocol/ibb"));

void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-jet-omemo:") + QString::number(features.size()));
    disco.setFeatures(Features(features));
    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

QByteArray randomFile(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    for (auto &byte : data)
        byte = char(QRandomGenerator::global()->bounded(256));
    return data;
}

struct Transfer {
    QPointer<J::Session>      session;
    QPointer<J::Session>      peerSession;
    QPointer<FT::Application> sending;
    QPointer<FT::Application> receiving;
    std::unique_ptr<QBuffer>  source;
    std::unique_ptr<QBuffer>  sink;
    QByteArray                wire; // IBB payload as sent
    bool                      security = false;
    QString                   securityType;
    QString                   cipher;
    // Session completion may delete the applications before send() returns.
    bool                           offered            = false;
    bool                           sendingEncrypted   = false;
    bool                           receivingEncrypted = false;
    J::State                       receivingState     = J::State::Created;
    J::Reason                      receivingReason;
    J::Session::PeerAuthentication receiverAuthentication;
};

// Alice offers @a data to bob over IBB with the given policy; bob accepts.
std::unique_ptr<Transfer> send(Endpoint &alice, Endpoint &bob, const QByteArray &data, bool preferred, bool required,
                               const std::function<void(QDomElement &)> &tamper = { })
{
    auto t    = std::make_unique<Transfer>();
    t->source = std::make_unique<QBuffer>();
    t->source->setData(data);
    t->source->open(QIODevice::ReadOnly);
    t->sink = std::make_unique<QBuffer>();
    t->sink->open(QIODevice::WriteOnly);

    auto raw = t.get();
    onStanza = [raw, tamper](const Endpoint &, QDomElement &stanza) {
        if (tamper)
            tamper(stanza);
        const auto ibb = stanza.firstChildElement(QStringLiteral("data"));
        if (ibb.namespaceURI() == IbbNs)
            raw->wire += QByteArray::fromBase64(ibb.text().toLatin1());
        const auto found = stanza.firstChildElement(QStringLiteral("jingle"))
                               .elementsByTagNameNS(J::JetOmemo::jetNs(), QStringLiteral("security"));
        if (!found.isEmpty()) {
            raw->security     = true;
            raw->securityType = found.at(0).toElement().attribute(QStringLiteral("type"));
            raw->cipher       = found.at(0).toElement().attribute(QStringLiteral("cipher"));
        }
    };
    auto connection = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                       [raw](J::Session *session) { raw->peerSession = session; });

    FT::File file;
    file.setName(QStringLiteral("secret.bin"));
    file.setSize(quint64(data.size()));
    file.addHash(Hash::from(Hash::Sha256, data));

    t->session = alice.client.jingleManager()->newSession(bob.jid);
    t->session->setEndToEndPreferred(preferred);
    t->session->setEndToEndRequired(required);
    t->sending = qobject_cast<FT::Application *>(t->session->newContent(FT::NS, J::Origin::Initiator));
    t->sending->setFile(file);
    check(t->sending->setTransport(t->session->newOutgoingTransport(J::IBB::NS)), "IBB transport was not set");
    QObject::connect(t->sending, &FT::Application::deviceRequested, t->source.get(),
                     [raw](quint64, std::optional<quint64>) {
                         raw->sendingEncrypted = raw->sending->isTransportEncrypted();
                         raw->sending->setDevice(raw->source.get(), false);
                     });
    t->session->addContent(t->sending);
    t->session->initiate();
    for (int i = 0; i < 20 && !t->peerSession && t->session && t->session->state() != J::State::Finished; ++i)
        pump(alice, bob);
    QObject::disconnect(connection);
    if (!t->peerSession)
        return t;

    for (auto content : t->peerSession->contentList())
        if (auto received = qobject_cast<FT::Application *>(content))
            t->receiving = received;
    check(t->receiving, "the offered file is missing");
    t->offered = true;
    t->receiving->setAcceptFile(file);
    QObject::connect(t->receiving, &FT::Application::stateChanged, t->sink.get(), [raw](J::State state) {
        raw->receivingState         = state;
        raw->receivingReason        = raw->receiving->lastReason();
        raw->receivingEncrypted     = raw->receiving->isTransportEncrypted();
        raw->receiverAuthentication = raw->peerSession->peerAuthentication();
    });
    QObject::connect(t->receiving, &FT::Application::deviceRequested, t->sink.get(),
                     [raw](quint64, std::optional<quint64>) { raw->receiving->setDevice(raw->sink.get(), false); });
    t->peerSession->accept();
    for (int i = 0; i < 400 && t->receiving && t->receiving->state() < J::State::Finished; ++i)
        pump(alice, bob);
    return t;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;

    Endpoint alice(QStringLiteral("alice"));
    Endpoint bob(QStringLiteral("bob"));
    for (auto endpoint : { &alice, &bob }) {
        auto setUp = wait(endpoint->omemo->setUp(), alice, bob);
        check(setUp->success(), "OMEMO was not set up");
    }
    J::JetOmemo aliceJet(alice.client.jingleManager(), alice.omemo);
    J::JetOmemo bobJet(bob.client.jingleManager(), bob.omemo);
    if (!alice.client.jingleManager()->discoFeatures().contains(J::JetOmemo::ns())) {
        qInfo("JET-OMEMO tests skipped: no legacy OMEMO or AES-GCM/CTR in QCA");
        pep.clear();
        return 0;
    }
    const auto protocols = alice.omemo->supportedProtocols();
    for (const bool omemo2 : { false, true }) {
        if (!protocols.testFlag(omemo2 ? OmemoProtocol::Omemo2 : OmemoProtocol::Legacy))
            continue;
        QStringList legacy { OmemoEncryption::legacyDevicesNode() + QStringLiteral("+notify") };
        if (omemo2)
            legacy << OmemoEncryption::namespaceUri();
        setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures() + legacy);
        setPeerFeatures(bob.client, alice.jid, alice.client.jingleManager()->discoFeatures() + legacy);

        // Encrypted: the plain file arrives, the wire carries ciphertext and the tag.
        const auto data = randomFile(100000);
        auto       t    = send(alice, bob, data, true, false);
        check(t->offered, "the encrypted offer did not arrive");
        check(t->security && t->sendingEncrypted && t->receivingEncrypted, "the file was not offered with JET");
        check(t->securityType == (omemo2 ? OmemoEncryption::namespaceUri() : OmemoEncryption::legacyNamespaceUri())
                  && t->cipher.contains(omemo2 ? QLatin1String("aes-256") : QLatin1String("aes-128")),
              "the JET envelope does not use the expected OMEMO profile and cipher");
        check(t->sink->data() == data, "the decrypted file differs");
        check(t->wire.size() == data.size() + 16 && !t->wire.contains(data.left(64)),
              "the wire did not carry ciphertext with the tag");
        check(t->receiverAuthentication.deviceId == alice.omemo->ownDeviceId(),
              "the receiver did not record the sender's device");

        // A tampered block fails authentication.
        t = send(alice, bob, data, true, false, [](QDomElement &stanza) {
            auto ibb = stanza.firstChildElement(QStringLiteral("data"));
            if (ibb.namespaceURI() != IbbNs || ibb.attribute(QStringLiteral("seq")) != QLatin1String("1"))
                return;
            auto bytes = QByteArray::fromBase64(ibb.text().toLatin1());
            bytes[0]   = char(bytes[0] ^ 1);
            auto text  = ibb.firstChild().toText();
            text.setData(QString::fromLatin1(bytes.toBase64()));
        });
        check(t->offered && t->receivingState == J::State::Finished
                  && t->receivingReason.condition() == J::Reason::SecurityError,
              "a tampered file was not refused");

        // Preferred, but the peer has no JET-OMEMO: the file goes unprotected.
        setPeerFeatures(alice.client, bob.jid, QStringList { QStringLiteral("urn:xmpp:jingle:1") } + legacy);
        t = send(alice, bob, data, true, false);
        check(t->offered && !t->security && !t->receivingEncrypted && t->sink->data() == data,
              "a preferred encryption blocked a transfer to a peer without JET-OMEMO");

        // Required, but the peer has no JET-OMEMO: nothing is offered.
        t = send(alice, bob, data, true, true);
        check(!t->peerSession && t->wire.isEmpty() && (!t->session || t->session->state() == J::State::Finished),
              "a required encryption sent the file to a peer without JET-OMEMO");
        qInfo("JET-OMEMO %s passed", omemo2 ? "2" : "legacy");
    }

    onStanza = nullptr;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("JET-OMEMO tests passed");
    pep.clear();
    return 0;
}
