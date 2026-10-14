// SPDX-License-Identifier: LGPL-2.1-or-later
// OmemoEncryption::encryptElement()/decryptElement() between two clients in both wire profiles, as used
// for OMEMO-verified DTLS fingerprints and JET envelopes in Jingle. A small in-memory PEP service stands
// in for the servers.
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_encryption.h>
#include <iris/xmpp_omemo.h>
#include <iris/xmpp_omemostorage.h>
#include <iris/xmpp_task.h>

#include <QCoreApplication>
#include <QMap>
#include <QPointer>
#include <QTextStream>
#include <QtCrypto>
#include <functional>

using namespace XMPP;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

namespace {
const QString PubSubNs(QStringLiteral("http://jabber.org/protocol/pubsub"));
const QString DtlsNs(QStringLiteral("urn:xmpp:jingle:apps:dtls:0"));

class NoNetworkConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class RecordingStream final : public ClientStream {
public:
    explicit RecordingStream(Connector *connector) : ClientStream(connector) { }
    // The stanza's own document may die with it: keep a copy in a document of the stream.
    void write(const Stanza &stanza) override
    {
        written.append(document.importNode(stanza.element(), true).toElement());
    }

    QDomDocument       document;
    QList<QDomElement> written;
};

struct Endpoint {
    NoNetworkConnector           connector;
    RecordingStream              stream { &connector };
    Client                       client;
    Jid                          jid;
    MemoryOmemoStorage           storage;
    MemoryEncryptionTrustStorage trust;
    OmemoEncryption             *omemo = nullptr;

    explicit Endpoint(const QString &user)
    {
        jid = Jid(user + QStringLiteral("@example.test/device"));
        client.start(QStringLiteral("example.test"), user, QString(), QStringLiteral("device"));
        client.connectToServer(&stream, jid);
        omemo = new OmemoEncryption(&client, &storage, &trust);
    }
    // The method uses the storages, which are destroyed before the client.
    ~Endpoint() { delete omemo; }
};

// PEP nodes of all accounts: owner bare JID → node → item id → payload.
QMap<QString, QMap<QString, QMap<QString, QDomElement>>> pep;
QDomDocument                                             pepDocument;
// Like a server whose node keeps one item or that ignores the requested item ids.
bool pepIgnoresItemIds = false;

QDomElement reply(const QDomElement &iq, const QString &type)
{
    auto result = pepDocument.createElement(QStringLiteral("iq"));
    result.setAttribute(QStringLiteral("type"), type);
    result.setAttribute(QStringLiteral("id"), iq.attribute(QStringLiteral("id")));
    result.setAttribute(QStringLiteral("to"), iq.attribute(QStringLiteral("from")));
    const auto to = iq.attribute(QStringLiteral("to"));
    if (!to.isEmpty())
        result.setAttribute(QStringLiteral("from"), to);
    return result;
}

QDomElement error(const QDomElement &iq, const QString &condition)
{
    auto result = reply(iq, QStringLiteral("error"));
    auto err    = pepDocument.createElement(QStringLiteral("error"));
    err.setAttribute(QStringLiteral("type"), QStringLiteral("cancel"));
    err.appendChild(pepDocument.createElementNS(QStringLiteral("urn:ietf:params:xml:ns:xmpp-stanzas"), condition));
    result.appendChild(err);
    return result;
}

// Answers an IQ addressed to an account (PEP) or to the server.
QDomElement serve(const QDomElement &iq, const Jid &sender)
{
    const Jid  to(iq.attribute(QStringLiteral("to")));
    const auto owner  = to.isValid() && !to.node().isEmpty() ? to.bare() : sender.bare();
    const auto type   = iq.attribute(QStringLiteral("type"));
    const auto pubsub = iq.firstChildElement(QStringLiteral("pubsub"));
    if (pubsub.namespaceURI() == PubSubNs && type == QLatin1String("set")) {
        const auto publish = pubsub.firstChildElement(QStringLiteral("publish"));
        if (!publish.isNull() && owner == sender.bare()) {
            const auto node = publish.attribute(QStringLiteral("node"));
            const auto item = publish.firstChildElement(QStringLiteral("item"));
            auto       id   = item.attribute(QStringLiteral("id"));
            if (id.isEmpty())
                id = QStringLiteral("current");
            pep[owner][node][id] = pepDocument.importNode(item.firstChildElement(), true).toElement();
            auto result          = reply(iq, QStringLiteral("result"));
            auto ps              = pepDocument.createElementNS(PubSubNs, QStringLiteral("pubsub"));
            auto pub             = pepDocument.createElementNS(PubSubNs, QStringLiteral("publish"));
            pub.setAttribute(QStringLiteral("node"), node);
            auto it = pepDocument.createElementNS(PubSubNs, QStringLiteral("item"));
            it.setAttribute(QStringLiteral("id"), id);
            pub.appendChild(it);
            ps.appendChild(pub);
            result.appendChild(ps);
            return result;
        }
        const auto retract = pubsub.firstChildElement(QStringLiteral("retract"));
        if (!retract.isNull() && owner == sender.bare()) {
            pep[owner][retract.attribute(QStringLiteral("node"))].remove(
                retract.firstChildElement(QStringLiteral("item")).attribute(QStringLiteral("id")));
            return reply(iq, QStringLiteral("result"));
        }
    }
    if (pubsub.namespaceURI() == PubSubNs && type == QLatin1String("get")) {
        const auto items = pubsub.firstChildElement(QStringLiteral("items"));
        const auto node  = items.attribute(QStringLiteral("node"));
        if (!pep.value(owner).contains(node))
            return error(iq, QStringLiteral("item-not-found"));
        QStringList wanted;
        for (auto item = items.firstChildElement(QStringLiteral("item")); !item.isNull();
             item      = item.nextSiblingElement(QStringLiteral("item")))
            wanted.append(item.attribute(QStringLiteral("id")));
        auto result = reply(iq, QStringLiteral("result"));
        auto ps     = pepDocument.createElementNS(PubSubNs, QStringLiteral("pubsub"));
        auto list   = pepDocument.createElementNS(PubSubNs, QStringLiteral("items"));
        list.setAttribute(QStringLiteral("node"), node);
        const auto stored = pep.value(owner).value(node);
        for (auto it = stored.cbegin(); it != stored.cend(); ++it) {
            if (!pepIgnoresItemIds && !wanted.isEmpty() && !wanted.contains(it.key()))
                continue;
            auto item = pepDocument.createElementNS(PubSubNs, QStringLiteral("item"));
            item.setAttribute(QStringLiteral("id"), it.key());
            item.appendChild(it.value().cloneNode(true));
            list.appendChild(item);
        }
        ps.appendChild(list);
        result.appendChild(ps);
        return result;
    }
    if (type == QLatin1String("set") && pubsub.namespaceURI() == PubSubNs + QStringLiteral("#owner"))
        return reply(iq, QStringLiteral("result")); // node configuration
    if (type == QLatin1String("result") || type == QLatin1String("error"))
        return {};
    return error(iq, QStringLiteral("service-unavailable"));
}

// Sees, and may change, every stanza on its way; for tests that include this fixture.
std::function<void(const Endpoint &from, QDomElement &stanza)> onStanza;

// Delivers stanzas between the endpoints and answers PEP and server IQs until everything is quiet.
void pump(Endpoint &a, Endpoint &b)
{
    for (int round = 0; round < 500; ++round) {
        bool moved = false;
        for (auto [from, to] : { std::pair<Endpoint *, Endpoint *> { &a, &b }, { &b, &a } }) {
            auto out = std::move(from->stream.written);
            from->stream.written.clear();
            for (auto stanza : out) {
                moved = true;
                stanza.setAttribute(QStringLiteral("from"), from->jid.full());
                if (onStanza)
                    onStanza(*from, stanza);
                if (qEnvironmentVariableIsSet("OMEMO_TEST_DUMP")) {
                    QString     xml;
                    QTextStream ts(&xml);
                    stanza.save(ts, -1);
                    qInfo("%s -> %s", qPrintable(from->jid.node()), qPrintable(xml.left(600)));
                }
                const Jid target(stanza.attribute(QStringLiteral("to")));
                if (stanza.tagName() == QLatin1String("iq") && (!target.isValid() || target.resource().isEmpty())) {
                    const auto answer = serve(stanza, from->jid);
                    if (qEnvironmentVariableIsSet("OMEMO_TEST_DUMP") && !answer.isNull()) {
                        QString     xml;
                        QTextStream ts(&xml);
                        answer.save(ts, -1);
                        qInfo("pep -> %s", qPrintable(xml.left(700)));
                    }
                    if (!answer.isNull())
                        from->client.rootTask()->take(answer);
                    continue;
                }
                if (target.isValid() && target.bare() == to->jid.bare())
                    to->client.rootTask()->take(stanza);
            }
        }
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        if (!moved && a.stream.written.isEmpty() && b.stream.written.isEmpty())
            return;
    }
    qFatal("stanza exchange did not settle");
}

EncryptionJob *wait(EncryptionJob *job, Endpoint &a, Endpoint &b)
{
    for (int i = 0; i < 50 && !job->isFinished(); ++i)
        pump(a, b);
    check(job->isFinished(), "an OMEMO job did not finish");
    if (!job->success())
        qWarning("OMEMO job failed: %s", qPrintable(job->errorString()));
    return job;
}

QDomElement fingerprint(QDomDocument &document)
{
    auto element = document.createElementNS(DtlsNs, QStringLiteral("fingerprint"));
    element.setAttribute(QStringLiteral("hash"), QStringLiteral("sha-256"));
    element.setAttribute(QStringLiteral("setup"), QStringLiteral("actpass"));
    element.appendChild(document.createTextNode(QStringLiteral("AB:CD:EF:01:23")));
    document.appendChild(element);
    return element;
}

int keyCount(const QDomElement &encrypted)
{
    int count = 0;
    for (auto header = encrypted.firstChildElement(QStringLiteral("header")); !header.isNull();
         header      = header.nextSiblingElement(QStringLiteral("header"))) {
        count += header.elementsByTagName(QStringLiteral("key")).size();
    }
    return count;
}

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
}

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
