// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_encryption.h>

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QEventLoop>
#include <QHash>
#include <QPointer>
#include <QTimer>

#include <cstdlib>

using namespace XMPP;

static void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
        std::abort();
    }
}

class StreamSession final : public EncryptedSession {
public:
    explicit StreamSession(const EncryptionContext &ctx) : EncryptedSession(QStringLiteral("test-stream"), ctx) { }
    int               stanzaCalls = 0;
    EncryptionContext lastContext;

    EncryptionJob *doEncrypt(const QDomElement &xml, const EncryptionContext &ctx) override
    {
        ++stanzaCalls;
        lastContext = ctx;
        auto *job   = new EncryptionJob(this);
        job->complete(xml);
        return job;
    }

    EncryptionJob *doDecrypt(const QDomElement &xml, const EncryptionContext &ctx) override
    {
        ++stanzaCalls;
        lastContext = ctx;
        auto *job   = new EncryptionJob(this);
        job->complete(xml);
        return job;
    }

    EncryptionJob *doEncryptChunk(const QByteArray &chunk, bool final) override
    {
        auto *job = new EncryptionJob(this);
        QTimer::singleShot(0, job, [job, chunk, final]() {
            job->complete(chunk.toUpper() + (final ? QByteArray("TAG") : QByteArray()));
        });
        return job;
    }

    EncryptionJob *doDecryptChunk(const QByteArray &chunk, bool final) override
    {
        auto *job = new EncryptionJob(this);
        QTimer::singleShot(0, job, [job, chunk, final]() {
            if (final && !chunk.endsWith("TAG"))
                job->fail(EncryptionJob::Error::AuthenticationFailed, QStringLiteral("Bad stream tag"));
            else
                job->complete(final ? chunk.left(chunk.size() - 3).toLower() : chunk.toLower());
        });
        return job;
    }
};

class ConcurrentContextSession final : public EncryptedSession {
public:
    ConcurrentContextSession() : EncryptedSession(QStringLiteral("test-concurrent")) { }

    EncryptionJob *doEncrypt(const QDomElement &xml, const EncryptionContext &context) override
    {
        auto              *job = new EncryptionJob(this);
        EncryptionMetadata metadata;
        metadata.details.insert(QStringLiteral("operation"), context.options.value(QStringLiteral("operation")));
        QTimer::singleShot(0, job, [job, xml, metadata]() { job->complete(xml, metadata); });
        return job;
    }
};

class StreamMethod final : public EncryptionMethod {
public:
    QString           id() const override { return QStringLiteral("test-stream"); }
    QString           name() const override { return QStringLiteral("Test stream"); }
    Capabilities      capabilities() const override { return XmppStanza | DataStream; }
    EncryptedSession *startSession(Capabilities caps, const EncryptionContext &ctx) override
    {
        return caps ? new StreamSession(ctx) : nullptr;
    }
    Features     features() const override { return {}; }
    bool         canDecrypt(const QDomElement &) const override { return false; }
    Availability availability(const Jid &peer) const override
    {
        return peer.bare() == QStringLiteral("unsupported@example.test") ? Availability::Unavailable
                                                                         : Availability::Available;
    }
};

// Pending incoming jobs must remain independent; a second stanza cannot
// cancel the first. Disconnect, however, invalidates every unfinished job.
class TestConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};

class TestStream final : public ClientStream {
public:
    explicit TestStream(Connector *connector) : ClientStream(connector) { }
    void   write(const Stanza &) override { }
    bool   stanzaAvailable() const override { return !incoming.isEmpty(); }
    Stanza read() override { return incoming.takeFirst(); }
    void   inject(const QDomElement &xml)
    {
        incoming.append(createStanza(xml));
        emit readyRead();
    }
    QList<Stanza> incoming;
};

class DeferredMethod final : public EncryptionMethod {
public:
    struct Operation {
        QPointer<EncryptionJob> job;
        QDomDocument            stanza;
    };
    QList<Operation> pending;
    QString          id() const override { return QStringLiteral("test-deferred-incoming"); }
    QString          name() const override { return id(); }
    Capabilities     capabilities() const override { return XmppStanza; }
    Features         features() const override { return {}; }
    bool             canDecrypt(const QDomElement &stanza) const override
    {
        return stanza.firstChildElement(QStringLiteral("encrypted")).namespaceURI()
            == QLatin1String("urn:psi:test:deferred-incoming");
    }

    class Session final : public EncryptedSession {
    public:
        Session(DeferredMethod *method, const EncryptionContext &base) :
            EncryptedSession(method->id(), base), method(method)
        {
        }
        EncryptionJob *doDecrypt(const QDomElement &stanza, const EncryptionContext &) override
        {
            auto     *job = new EncryptionJob(this);
            Operation operation;
            operation.job = job;
            operation.stanza.appendChild(operation.stanza.importNode(stanza, true));
            method->pending.append(operation);
            return job;
        }

    private:
        DeferredMethod *method;
    };

    EncryptedSession *startSession(Capabilities caps, const EncryptionContext &context) override
    {
        return caps.testFlag(XmppStanza) ? new Session(this, context) : nullptr;
    }
};

static void exercisePendingIncomingCancellation()
{
    Client         client;
    TestConnector  connector;
    TestStream     stream(&connector);
    DeferredMethod method;
    client.start(QStringLiteral("example.test"), QStringLiteral("bob"), {}, QStringLiteral("device"));
    client.connectToServer(&stream, Jid(QStringLiteral("bob@example.test/device")));
    check(client.encryptionManager()->registerMethod(&method), "Could not register deferred decrypt method");
    client.setIncomingEncryptedStanzaHandler([&client](const QDomElement &stanza, const QString &methodId,
                                                       const EncryptionContext &context,
                                                       Client::EncryptionSessionPurpose) -> EncryptionJob * {
        auto *session = client.encryptionManager()->startSession(methodId, EncryptionMethod::XmppStanza);
        if (!session)
            return nullptr;
        auto *job = session->decrypt(stanza, context);
        session->close();
        return job;
    });
    int delivered = 0;
    QObject::connect(&client, &Client::messageReceived, &client, [&](const Message &) { ++delivered; });

    const auto inject = [&](const QString &body) {
        QDomDocument doc;
        auto         message = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("message"));
        doc.appendChild(message);
        message.setAttribute(QStringLiteral("type"), QStringLiteral("chat"));
        message.setAttribute(QStringLiteral("from"), QStringLiteral("alice@example.test/desktop"));
        message.setAttribute(QStringLiteral("to"), QStringLiteral("bob@example.test/device"));
        auto encrypted
            = doc.createElementNS(QStringLiteral("urn:psi:test:deferred-incoming"), QStringLiteral("encrypted"));
        message.appendChild(encrypted);
        auto content = doc.createElementNS(QStringLiteral("jabber:client"), QStringLiteral("body"));
        content.appendChild(doc.createTextNode(body));
        message.appendChild(content);
        stream.inject(message);
    };

    inject(QStringLiteral("one"));
    inject(QStringLiteral("two"));
    check(method.pending.size() == 2 && delivered == 0, "Concurrent incoming decrypt jobs did not start");
    for (int i = 0; i < 2; ++i) {
        auto &operation = method.pending[i];
        check(operation.job && !operation.job->isFinished(), "Pending incoming job was cancelled by a newer stanza");
        operation.job->complete(operation.stanza.documentElement());
    }
    check(delivered == 2, "Both incoming decrypt operations must deliver");

    inject(QStringLiteral("obsolete"));
    check(method.pending.size() == 3 && method.pending.last().job,
          "Could not start a pending decryption before disconnect");
    QPointer<EncryptionJob> obsolete = method.pending.last().job;
    client.close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(obsolete.isNull() && delivered == 2, "Disconnect did not cancel the pending decryption job");
}

static EncryptionJob *await(EncryptionJob *job)
{
    if (!job->isFinished()) {
        QEventLoop loop;
        QObject::connect(job, &EncryptionJob::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return job;
}

int main(int argc, char **argv)
{
    QCoreApplication  app(argc, argv);
    EncryptionManager manager;
    StreamMethod      method;
    check(manager.registerMethod(&method), "Cannot register stream method");
    check(manager.methods().count(method.id()) == 1, "Default registry query returned no methods");
    check(manager.availability(method.id(), Jid(QStringLiteral("online@example.test")))
              == EncryptionMethod::Availability::Available,
          "Registered method availability failed");
    check(manager.availability(method.id(), Jid(QStringLiteral("unsupported@example.test")))
              == EncryptionMethod::Availability::Unavailable,
          "Method-specific availability was ignored");
    check(manager.availability(QStringLiteral("missing"), Jid(QStringLiteral("online@example.test")))
              == EncryptionMethod::Availability::Unavailable,
          "Missing method was treated as available");
    const auto signalingAndStream
        = EncryptionMethod::Capabilities(EncryptionMethod::XmppStanza | EncryptionMethod::DataStream);
    check(manager.methods(signalingAndStream).count(method.id()) == 1,
          "Method supporting all required capabilities was not selected");
    check(manager.methods(EncryptionMethod::XmppStanza | EncryptionMethod::DataMessage).empty(),
          "Registry selected a method supporting only part of the required capabilities");

    // The host owns peer sessions: different resources may use different
    // sessions, and Iris never resolves or caches them by JID.
    {
        Client       client;
        StreamMethod registeredMethod;
        check(client.encryptionManager()->registerMethod(&registeredMethod),
              "Could not register a peer-specific method");
        const Jid         peerA(QStringLiteral("alice@example.test/desktop"));
        const Jid         peerB(QStringLiteral("alice@example.test/mobile"));
        EncryptionContext peerAContext;
        peerAContext.recipients = { peerA };
        EncryptionContext peerBContext;
        peerBContext.recipients = { peerB };
        auto *sessionA = client.encryptionManager()->startSession(registeredMethod.id(), EncryptionMethod::XmppStanza,
                                                                  peerAContext);
        auto *sessionB = client.encryptionManager()->startSession(registeredMethod.id(), EncryptionMethod::XmppStanza,
                                                                  peerBContext);
        check(sessionA && sessionB && sessionA != sessionB, "Separate host-owned peer sessions were not created");

        QDomDocument testDoc;
        auto         testStanza = testDoc.createElement(QStringLiteral("message"));
        testDoc.appendChild(testStanza);
        EncryptionContext operation;
        operation.options.insert(QStringLiteral("perMessage"), QStringLiteral("first"));
        operation.replyTo           = EncryptionMetadata {};
        operation.replyTo->methodId = registeredMethod.id();
        auto *session               = dynamic_cast<StreamSession *>(sessionA);
        check(session, "Test session implementation not returned");
        check(await(sessionA->encrypt(testStanza, operation))->success(),
              "The peer session could not encrypt its first stanza");
        check(session->lastContext.options.value(QStringLiteral("perMessage")).toString() == QStringLiteral("first")
                  && session->lastContext.replyTo.has_value()
                  && session->lastContext.recipients == QList<Jid> { peerA },
              "The per-stanza context or bound peer was lost");
        check(await(sessionA->decrypt(testStanza))->success() && session->stanzaCalls == 2,
              "The host-owned session was not reused for incoming messages");
        check(!session->lastContext.options.contains(QStringLiteral("perMessage"))
                  && !session->lastContext.replyTo.has_value(),
              "An earlier stanza's context leaked into the next operation");

        // Explicit session calls reuse that session. Selecting only a method
        // creates a separate one-shot session and never consults host state.
        const int previousCalls = session->stanzaCalls;
        check(await(client.sendEncrypted(testStanza, sessionA))->success() && session->stanzaCalls == previousCalls + 1,
              "Explicitly bound outgoing encryption did not reuse its session");
        check(await(client.sendEncrypted(testStanza, registeredMethod.id(), peerBContext))->success()
                  && session->stanzaCalls == previousCalls + 1,
              "Method-only encryption unexpectedly reused a host-owned session");

        // The host, not Client, controls when peer sessions are retired.
        client.close();
        check(!sessionA->isClosing() && !sessionB->isClosing(), "Client retired host-owned sessions on disconnect");
        sessionA->close();
        check(sessionA->isClosing() && !sessionB->isClosing(),
              "Retiring one peer session affected a different resource");
        sessionB->close();
    }

    // Two concurrent stanza jobs must retain their own immutable operation
    // contexts even though they run through the same long-lived session.
    ConcurrentContextSession concurrent;
    EncryptionContext        firstContext;
    firstContext.options.insert(QStringLiteral("operation"), QStringLiteral("first"));
    EncryptionContext secondContext;
    secondContext.options.insert(QStringLiteral("operation"), QStringLiteral("second"));
    QDomDocument concurrentDocument;
    auto         concurrentMessage = concurrentDocument.createElement(QStringLiteral("message"));
    concurrentDocument.appendChild(concurrentMessage);
    auto *firstJob  = concurrent.encrypt(concurrentMessage, firstContext);
    auto *secondJob = concurrent.encrypt(concurrentMessage, secondContext);
    check(!firstJob->isFinished() && !secondJob->isFinished(), "Concurrent stanza jobs were unexpectedly synchronous");
    check(await(firstJob)->success() && await(secondJob)->success()
              && firstJob->metadata().details.value(QStringLiteral("operation")).toString() == QStringLiteral("first")
              && secondJob->metadata().details.value(QStringLiteral("operation")).toString() == QStringLiteral("second")
              && concurrent.context().options.isEmpty(),
          "Concurrent encrypted operations shared mutable stanza context");

    // Jingle can use one encryption session both for signaling XML and for payload chunks.
    auto *tx = manager.startSession(
        method.id(), EncryptionMethod::Capabilities(EncryptionMethod::XmppStanza | EncryptionMethod::DataStream));
    check(tx != nullptr, "Cannot start combined signaling/stream session");
    QDomDocument doc;
    auto         signaling = doc.createElement(QStringLiteral("jingle"));
    doc.appendChild(signaling);
    auto *signaled = await(tx->encrypt(signaling));
    check(signaled->success() && signaled->stanza().tagName() == QLatin1String("jingle"),
          "Shared session cannot protect signaling");
    auto *first = tx->encryptChunk(QByteArray("ab"));
    check(!first->isFinished() && tx->hasActiveOperations(), "Expected an active asynchronous chunk");
    auto *overlap = tx->encryptChunk(QByteArray("cd"));
    check(overlap->isFinished() && !overlap->success(), "Overlapping chunks accepted");
    auto *wrongDirection = tx->decryptChunk(QByteArray("ab"));
    check(wrongDirection->isFinished() && !wrongDirection->success(), "Direction switch accepted");
    check(await(first)->success() && first->data() == "AB" && !tx->hasActiveOperations(),
          "First chunk failed or left the session active");
    auto *last = await(tx->encryptChunk(QByteArray("cd"), true));
    check(last->success() && last->data() == "CDTAG", "Final encryption chunk failed");
    check(!tx->encryptChunk(QByteArray("extra"))->success(), "Finalized stream reused");
    tx->close();

    auto *rx    = manager.startSession(method.id(), EncryptionMethod::DataStream);
    auto *plain = await(rx->decryptChunk(QByteArray("AB")));
    check(plain->success() && plain->data() == "ab", "Intermediate decryption failed");
    auto *tampered = await(rx->decryptChunk(QByteArray("CDbad"), true));
    check(!tampered->success() && tampered->error() == EncryptionJob::Error::AuthenticationFailed,
          "Tampered final tag accepted");
    check(!rx->decryptChunk(QByteArray("TAG"), true)->success(), "Failed stream reused");
    rx->close();

    auto *ok       = manager.startSession(method.id(), EncryptionMethod::DataStream);
    auto *verified = await(ok->decryptChunk(QByteArray("XYZTAG"), true));
    check(verified->success() && verified->data() == "xyz", "Valid stream not finalized");
    ok->close();

    auto *stanzaOnly = manager.startSession(method.id(), EncryptionMethod::XmppStanza);
    check(stanzaOnly != nullptr, "Stanza-only session not created");
    auto *wrongCapability = stanzaOnly->encryptChunk(QByteArray("plain"));
    check(wrongCapability->isFinished() && !wrongCapability->success(),
          "Stream operation accepted on a stanza-only session");
    stanzaOnly->close();
    check(!manager.startSession(method.id(), EncryptionMethod::DataMessage),
          "Unsupported data-message capability accepted");
    // Methods also create short-lived sessions directly (e.g. OMEMO for Jingle elements).
    // They must retain the same public encrypt() API without a registry-owned session.
    auto *directSession = method.startSession(EncryptionMethod::XmppStanza, {});
    check(directSession != nullptr, "Direct method session creation failed");
    auto *directResult = await(directSession->encrypt(signaling));
    check(directResult->success(), "Direct method session encryption failed");
    directSession->close();

    auto *registrySession = manager.startSession(method.id(), EncryptionMethod::DataStream);
    check(registrySession != nullptr, "Manager failed to create a session for unload test");
    manager.unregisterMethod(&method);
    check(registrySession->isClosing(), "Unregistering a method did not close its sessions");
    check(!registrySession->encryptChunk(QByteArray("more"))->success(), "A closed session accepted another operation");
    exercisePendingIncomingCancellation();
    qInfo("Stream session routing, pending-job cancellation and final authentication passed");
    return 0;
}
