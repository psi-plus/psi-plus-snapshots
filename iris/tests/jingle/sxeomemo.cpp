// SPDX-License-Identifier: LGPL-2.1-or-later
// A whiteboard on the SXE message transport with OMEMO 2 protected payload (SxeOmemo) between two
// clients: agreement in the transport, only markers and OMEMO envelopes on the wire, refused
// tampered and unprotected payload, and the session policy without peer support. Reuses the OMEMO
// fixture with its in-memory PEP service.
#define main iris_omemoelement_main
#include "omemoelement.cpp"
#undef main

#include <iris/jingle-omemo-verification.h>
#include <iris/jingle-session.h>
#include <iris/jingle-sxe-omemo.h>
#include <iris/xmpp-im/jingle-sxe.h>
#include <iris/xmpp_caps.h>

namespace J   = XMPP::Jingle;
namespace SXE = XMPP::Jingle::SXE;

namespace {
const QString SxeNs(QStringLiteral("urn:xmpp:sxe:0"));

void setPeerFeatures(Client &client, const Jid &peer, QStringList features)
{
    features.removeDuplicates();
    features.removeAll(SXE::NS_CONNECTIONS); // keep the message transport
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-sxe-omemo:") + QString::number(features.size()));
    disco.setFeatures(Features(features));
    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

struct Wire {
    int     markers = 0; // <sxe/> without children next to an OMEMO 2 envelope
    int     plain   = 0; // <sxe/> with edits in clear
    QString encryption;  // of the SXE <transport/>
};

struct Run {
    QPointer<J::Session>       session;
    QPointer<J::Session>       peerSession;
    QPointer<SXE::Application> local;
    QPointer<SXE::Application> remote;
};

bool synchronized(const Run &run)
{
    return run.local && run.remote && run.local->syncState() == SXE::Application::SyncState::Synchronized
        && run.remote->syncState() == SXE::Application::SyncState::Synchronized;
}

void parseInto(QDomDocument &doc, const QString &text)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    doc.setContent(text, true);
#else
    doc.setContent(text, QDomDocument::ParseOption::UseNamespaceProcessing);
#endif
}

int              droppedUnprotected = 0;
QtMessageHandler defaultHandler     = nullptr;
void             countDrops(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.contains(QLatin1String("dropping unprotected payload")))
        ++droppedUnprotected;
    defaultHandler(type, context, message);
}

QString dump(const QDomDocument &doc)
{
    QString     out;
    QTextStream stream(&out);
    doc.documentElement().save(stream, -1);
    return out;
}

Run start(Endpoint &alice, Endpoint &bob, bool preferred, bool required, Wire &wire,
          const std::function<void(QDomElement &)> &tamper = {})
{
    Run run;
    onStanza = [&wire, tamper](const Endpoint &, QDomElement &stanza) {
        if (tamper)
            tamper(stanza);
        const auto sxe = stanza.firstChildElement(QStringLiteral("sxe"));
        if (stanza.tagName() == QLatin1String("message") && sxe.namespaceURI() == SxeNs) {
            const bool envelope
                = !stanza.elementsByTagNameNS(OmemoEncryption::namespaceUri(), QStringLiteral("encrypted")).isEmpty();
            if (sxe.firstChildElement().isNull() && envelope)
                ++wire.markers;
            else if (!sxe.firstChildElement().isNull())
                ++wire.plain;
        }
        const auto transports
            = stanza.elementsByTagNameNS(QStringLiteral("urn:xmpp:jingle:transports:sxe"), QStringLiteral("transport"));
        if (!transports.isEmpty() && transports.at(0).toElement().hasAttribute(QStringLiteral("encryption")))
            wire.encryption = transports.at(0).toElement().attribute(QStringLiteral("encryption"));
    };
    auto connection = QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client,
                                       [&run](J::Session *session) { run.peerSession = session; });
    run.session     = alice.client.jingleManager()->newSession(bob.jid);
    run.session->setEndToEndPreferred(preferred);
    run.session->setEndToEndRequired(required);
    run.local = qobject_cast<SXE::Application *>(run.session->newContent(SXE::NS_SVG_WHITEBOARD));
    run.session->addContent(run.local);
    run.session->initiate();
    for (int i = 0; i < 20 && !run.peerSession && run.session && run.session->state() != J::State::Finished; ++i)
        pump(alice, bob);
    QObject::disconnect(connection);
    if (run.peerSession) {
        for (auto content : run.peerSession->contentList())
            run.remote = qobject_cast<SXE::Application *>(content);
        run.remote->prepare();
        run.peerSession->accept();
        for (int i = 0; i < 50 && !synchronized(run); ++i)
            pump(alice, bob);
    }
    return run;
}

void draw(Endpoint &alice, Endpoint &bob, const Run &run, const QString &id)
{
    QDomDocument temp;
    parseInto(temp, QStringLiteral("<rect xmlns='http://www.w3.org/2000/svg' id='%1' width='5' height='5'/>").arg(id));
    run.local->document()->insertNodeAfter(temp.documentElement(), run.local->document()->document().documentElement());
    run.local->document()->flush();
    for (int i = 0; i < 20; ++i)
        pump(alice, bob);
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
    if (!alice.omemo->supportedProtocols().testFlag(OmemoProtocol::Omemo2)) {
        qInfo("SXE OMEMO tests skipped: no OMEMO 2");
        pep.clear();
        return 0;
    }
    SXE::SxeOmemo            aliceSxe(alice.client.jingleManager(), alice.omemo);
    SXE::SxeOmemo            bobSxe(bob.client.jingleManager(), bob.omemo);
    J::OmemoDtlsVerification aliceVerification(alice.client.jingleManager(), alice.omemo);
    J::OmemoDtlsVerification bobVerification(bob.client.jingleManager(), bob.omemo);
    const QStringList        omemo2 { OmemoEncryption::namespaceUri() };
    check(alice.client.jingleManager()->discoFeatures().contains(SXE::SxeOmemo::feature()),
          "the protected SXE payload is not advertised");
    setPeerFeatures(alice.client, bob.jid, bob.client.jingleManager()->discoFeatures() + omemo2);
    setPeerFeatures(bob.client, alice.jid, alice.client.jingleManager()->discoFeatures() + omemo2);

    // Protected: the transport carries the method, the wire only markers and OMEMO 2 envelopes.
    Wire wire;
    auto run = start(alice, bob, true, false, wire);
    check(synchronized(run), "the protected whiteboard did not synchronize");
    draw(alice, bob, run, QStringLiteral("r1"));
    check(dump(run.remote->document()->document()) == dump(run.local->document()->document()),
          "edits of the protected whiteboard were not exchanged");
    check(wire.encryption == OmemoEncryption::namespaceUri() && wire.markers > 0 && wire.plain == 0,
          "SXE payload of a protected session travelled in clear");

    // A forged unprotected edit is dropped.
    QDomDocument forged;
    defaultHandler = qInstallMessageHandler(countDrops);
    parseInto(
        forged,
        QStringLiteral("<message xmlns='jabber:client' from='%1' to='%2'><sxe xmlns='urn:xmpp:sxe:0' session='%3' "
                       "id='999'><state><new type='element' rid='forged' name='circle' "
                       "ns='http://www.w3.org/2000/svg' parent='%4'/></state></sxe></message>")
            .arg(alice.jid.full(), bob.jid.full(), run.session->sid(), QStringLiteral("unknown")));
    bob.client.rootTask()->take(forged.documentElement());
    pump(alice, bob);
    qInstallMessageHandler(defaultHandler);
    check(droppedUnprotected == 1
              && run.remote->document()->document().elementsByTagName(QStringLiteral("circle")).isEmpty(),
          "an unprotected edit reached a protected whiteboard");

    // A tampered envelope is dropped.
    const auto before = dump(run.remote->document()->document());
    onStanza          = [](const Endpoint &, QDomElement &stanza) {
        const auto payloads = stanza.elementsByTagNameNS(OmemoEncryption::namespaceUri(), QStringLiteral("payload"));
        if (stanza.tagName() != QLatin1String("message") || payloads.isEmpty())
            return;
        auto text  = payloads.at(0).firstChild().toText();
        auto bytes = QByteArray::fromBase64(text.data().toLatin1());
        bytes[0]   = char(bytes[0] ^ 1);
        text.setData(QString::fromLatin1(bytes.toBase64()));
    };
    draw(alice, bob, run, QStringLiteral("r2"));
    check(dump(run.remote->document()->document()) == before, "a tampered edit was applied");
    run.session->terminate(J::Reason::Success);
    for (int i = 0; i < 5; ++i)
        pump(alice, bob);

    // Preferred, but the peer cannot protect SXE payload: the whiteboard runs in clear.
    setPeerFeatures(alice.client, bob.jid, QStringList { QStringLiteral("urn:xmpp:jingle:1") } + omemo2);
    wire = {};
    run  = start(alice, bob, true, false, wire);
    check(synchronized(run) && wire.encryption.isEmpty() && wire.plain > 0,
          "a preferred protection blocked a whiteboard with a peer that cannot protect it");
    run.session->terminate(J::Reason::Success);
    for (int i = 0; i < 5; ++i)
        pump(alice, bob);

    // Required, but the peer cannot protect SXE payload: nothing is sent.
    wire = {};
    run  = start(alice, bob, true, true, wire);
    check(!run.peerSession && wire.plain == 0 && (!run.session || run.session->state() == J::State::Finished),
          "a required protection started a whiteboard in clear");

    onStanza = nullptr;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo("SXE OMEMO tests passed");
    pep.clear();
    return 0;
}
