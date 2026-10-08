// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QSet>
#include <algorithm>
#include <functional>
#include <iris/jingle-ft.h>
#include <iris/xmpp-im/jingle-tiebreaker.h>
#include <iris/xmpp-im/xmpp_features.h>
#include <memory>
#define private public
#include <iris/jingle-session.h>
#undef private
#include <iris/xmpp_client.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <qca.h>

#include <cstring>

using namespace XMPP;
namespace J  = XMPP::Jingle;
namespace FT = XMPP::Jingle::FileTransfer;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class TestConnection final : public J::Connection {
public:
    TestConnection() { open(QIODevice::ReadWrite); }

    J::TransportFeatures features() const override
    {
        return J::TransportFeature::Reliable | J::TransportFeature::Ordered | J::TransportFeature::DataOriented;
    }

    qint64 bytesAvailable() const override { return incoming_.size() + J::Connection::bytesAvailable(); }

    void feed(const QByteArray &data)
    {
        incoming_ += data;
        emit readyRead();
    }

    void closeDuringNextRead() { closeDuringRead_ = true; }

protected:
    qint64 writeData(const char *, qint64 size) override
    {
        emit bytesWritten(size);
        return size;
    }

    qint64 readDataInternal(char *data, qint64 maxSize) override
    {
        const auto size = qMin<qint64>(maxSize, incoming_.size());
        if (size <= 0)
            return 0;
        memcpy(data, incoming_.constData(), size_t(size));
        incoming_.remove(0, int(size));
        if (closeDuringRead_) {
            closeDuringRead_ = false;
            // Model a transport that reports peer close synchronously while
            // returning its final payload. Do not close QIODevice itself from
            // inside readDataInternal(): Qt's QIODevice implementation cannot
            // safely tear down its private read buffer before read() returns.
            emit connectionClosed();
        }
        return size;
    }

private:
    QByteArray incoming_;
    bool       closeDuringRead_ = false;
};

class TestTransportPad final : public J::TransportManagerPad {
public:
    explicit TestTransportPad(J::Session *session) : session_(session) { }
    J::Session          *session() const override { return session_; }
    QString              ns() const override { return QStringLiteral("urn:iris:test:ft"); }
    J::TransportManager *manager() const override { return nullptr; }

private:
    J::Session *session_;
};

class TestTransport final : public J::Transport {
public:
    TestTransport(const J::TransportManagerPad::Ptr &pad, J::Origin creator) :
        J::Transport(pad, creator), connection_(QSharedPointer<TestConnection>::create())
    {
    }

    void prepare() override
    {
        setState(J::State::ApprovedToSend);
        emit connection_->connected();
    }
    void                           start() override { setState(J::State::Active); }
    bool                           update(const QDomElement &) override { return true; }
    bool                           hasUpdates() const override { return false; }
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool = false) override { return {}; }
    bool                           isValid() const override { return true; }
    J::TransportFeatures           features() const override { return connection_->features(); }
    J::Connection::Ptr addChannel(J::TransportFeatures, const QString &, int = -1) override { return connection_; }
    QList<J::Connection::Ptr> channels() const override { return { connection_ }; }

    QSharedPointer<TestConnection> connection() const { return connection_; }

private:
    QSharedPointer<TestConnection> connection_;
};

class TestApplication final : public FT::Application {
public:
    using FT::Application::Application;
    using FT::Application::incomingRemove;
    void installTransport(const QSharedPointer<J::Transport> &transport) { _transport = transport; }
};

static FT::File testFile(quint64 size)
{
    FT::File file;
    file.setName(QStringLiteral("tail.bin"));
    file.setSize(size);
    return file;
}

static void receiveReceipt(FT::Pad *pad, const QString &name)
{
    QDomDocument doc;
    auto         info     = doc.createElement(QStringLiteral("jingle"));
    auto         received = doc.createElementNS(FT::NS, QStringLiteral("received"));
    received.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
    received.setAttribute(QStringLiteral("name"), name);
    info.appendChild(received);
    check(pad->incomingSessionInfo(info), "file receipt was not handled");
}

static void receiveChecksum(FT::Pad *pad, const QString &name, const Hash &hash)
{
    QDomDocument doc;
    auto         info     = doc.createElement(QStringLiteral("jingle"));
    auto         checksum = doc.createElementNS(FT::NS, QStringLiteral("checksum"));
    checksum.setAttribute(QStringLiteral("creator"), QStringLiteral("initiator"));
    checksum.setAttribute(QStringLiteral("name"), name);
    FT::File file;
    file.addHash(hash);
    checksum.appendChild(file.toXml(&doc));
    info.appendChild(checksum);
    check(pad->incomingSessionInfo(info), "File checksum was not handled");
}

static void exerciseSessionTermination(Client &client, bool partial, bool localFailure, J::Reason::Condition peerReason,
                                       bool deleteSession = false)
{
    auto *session
        = new J::Session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);
    QPointer<J::Session> sessionGuard(session);
    auto                 appPad
        = QSharedPointer<FT::Pad>(static_cast<FT::Pad *>(client.jingleManager()->applicationPad(session, FT::NS)));
    auto                     transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(session));
    auto                     transport    = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
    QPointer<TestConnection> connection(transport->connection().data());
    auto *transfer = new TestApplication(appPad, QStringLiteral("session-termination"), J::Origin::Initiator,
                                         J::Origin::Initiator);
    transfer->setFile(testFile(partial ? 65536 : 4));
    transfer->setAcceptFile(testFile(partial ? 65536 : 4));
    transfer->installTransport(transport);
    session->addContent(transfer);
    QPointer<TestApplication> appGuard(transfer);
    QBuffer                   source;
    source.setData(QByteArray(partial ? 65536 : 4, 'x'));
    source.open(QIODevice::ReadOnly);
    QObject::connect(transfer, &FT::Application::deviceRequested, transfer,
                     [&, transfer](quint64, std::optional<quint64>) { transfer->setDevice(&source, false); });
    transfer->prepare();
    // Deliberately leave the final bytesWritten notification queued. Success
    // termination can legitimately arrive before that notification.
    check(transfer->state() == J::State::Active, "session termination fixture did not become active");
    check(source.atEnd() != partial, "session termination fixture queued the wrong payload amount");
    if (localFailure)
        transfer->remove(J::Reason::MediaError, QStringLiteral("local integrity failure"));
    int       finished = 0;
    J::Reason result;
    QObject::connect(transfer, &J::Application::stateChanged, &client, [&](J::State state) {
        if (state != J::State::Finished)
            return;
        ++finished;
        result = transfer->lastReason();
        check(!transfer->connection(), "session termination retained the application data connection");
        if (deleteSession)
            delete session;
        else
            session->terminate(J::Reason::Success); // model a reentrant UI completion callback
    });
    QDomDocument doc;
    auto         info = doc.createElementNS(J::NS, QStringLiteral("jingle"));
    info.appendChild(J::Reason(peerReason).toXml(&doc));
    check(session->updateFromXml(J::Action::SessionTerminate, info), "session termination was not handled");
    const auto expected = localFailure                ? J::Reason::MediaError
        : partial && peerReason == J::Reason::Success ? J::Reason::FailedApplication
                                                      : peerReason;
    check(finished == 1 && result.isValid() && result.condition() == expected,
          "session termination lost its reason or overwrote a local/partial transfer failure");
    check(!connection->isOpen(), "session termination left its data stream open");
    check(!sessionGuard || sessionGuard->state() == J::State::Finished,
          "reentrant completion reopened a terminated session");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!sessionGuard && !appGuard, "session termination leaked the session or application");
    transport.reset();
    check(!connection, "session termination leaked the transport connection");
}

int main(int argc, char **argv)
{
    QCoreApplication eventLoop(argc, argv);
    QCA::Initializer qca;
    Client           client;
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);

    auto *rawPad = client.jingleManager()->applicationPad(&session, FT::NS);
    auto *ftPad  = dynamic_cast<FT::Pad *>(rawPad);
    check(ftPad, "client did not provide registered FT pad");
    QSharedPointer<FT::Pad> appPad(ftPad);

    // Fatal integrity errors remain actionable after payload completion entered
    // Finishing. They must schedule ContentRemove, never be overwritten by the
    // successful finalize timeout.
    {
        TestApplication transfer(appPad, QStringLiteral("fatal"), J::Origin::Initiator, J::Origin::Responder);
        transfer.setFile(testFile(4));
        transfer.setState(J::State::Finishing);
        int updates = 0;
        QObject::connect(&transfer, &J::Application::updated, &eventLoop, [&]() { ++updates; });

        transfer.remove(J::Reason::Condition::MediaError, QStringLiteral("checksum mismatch"));
        const auto update = transfer.evaluateOutgoingUpdate();
        check(updates == 1, "fatal Finishing error did not request signaling");
        check(update.action == J::Action::ContentRemove, "fatal Finishing error did not become content-remove");
        check(update.reason.condition() == J::Reason::Condition::MediaError, "fatal Finishing error lost its reason");
        check(transfer.lastReason().condition() == J::Reason::Condition::MediaError,
              "fatal Finishing error was not made irreversible");
    }

    // stateChanged is a synchronous reentrancy boundary. Deleting the
    // application when a known-size receiver reaches Finishing must not leave
    // beginPayloadFinishing() touching its destroyed Private afterwards.
    {
        auto  transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(&session));
        auto  transport    = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
        auto *transfer
            = new TestApplication(appPad, QStringLiteral("reentrant"), J::Origin::Initiator, J::Origin::Responder);
        transfer->setFile(testFile(4));
        transfer->setAcceptFile(testFile(4));
        transfer->installTransport(transport);

        QBuffer sink;
        sink.open(QIODevice::WriteOnly);
        QObject::connect(transfer, &FT::Application::deviceRequested, &eventLoop,
                         [transfer, &sink](quint64, std::optional<quint64>) { transfer->setDevice(&sink, false); });

        QPointer<TestApplication> guard(transfer);
        int                       prematureUpdates = 0;
        QObject::connect(transfer, &J::Application::updated, &eventLoop, [&]() { ++prematureUpdates; });
        QObject::connect(transfer, &J::Application::stateChanged, &eventLoop, [transfer](J::State state) {
            if (state == J::State::Finishing)
                delete transfer;
        });

        transfer->prepare();
        check(guard && transfer->state() == J::State::Active, "FT receiver did not become active");
        transport->connection()->closeDuringNextRead();
        transport->connection()->feed(QByteArrayLiteral("tail"));
        check(!guard, "FT application was not deleted from Finishing callback");
        check(prematureUpdates == 0, "synchronous final-read close was misclassified as truncated transfer");
        check(sink.data() == QByteArrayLiteral("tail"), "FT receiver lost the final payload before reentrant deletion");
    }

    // Exhausting the source does not prove receipt. Exercise the real finalize
    // timer callback without waiting for its thirty-second production interval.
    {
        auto            transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(&session));
        auto            transport    = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
        TestApplication transfer(appPad, QStringLiteral("receipt-timeout"), J::Origin::Initiator, J::Origin::Initiator);
        transfer.setFile(testFile(4));
        transfer.setAcceptFile(testFile(4));
        transfer.installTransport(transport);
        session.addContent(&transfer);
        QBuffer source;
        source.setData(QByteArrayLiteral("tail"));
        source.open(QIODevice::ReadOnly);
        QObject::connect(&transfer, &FT::Application::deviceRequested, &eventLoop,
                         [&](quint64, std::optional<quint64>) { transfer.setDevice(&source, false); });
        transfer.prepare();
        QCoreApplication::processEvents();
        check(transfer.state() == J::State::Finishing, "FT sender did not reach receipt wait");
        auto timer = transfer.findChild<QTimer *>();
        check(timer && timer->isActive(), "FT sender did not arm receipt timeout");
        check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection), "could not fire receipt timeout");
        const auto update = transfer.evaluateOutgoingUpdate();
        check(update.action == J::Action::ContentRemove, "receipt timeout did not remove failed transfer");
        check(transfer.lastReason().condition() == J::Reason::FailedApplication,
              "receipt timeout was reported as success");
        receiveReceipt(ftPad, transfer.contentName());
        check(transfer.lastReason().condition() == J::Reason::FailedApplication,
              "late receipt overwrote terminal transfer failure");
    }

    // Negotiating hash-used makes checksum confirmation mandatory. Receiving
    // the byte count cannot turn a missing checksum into success on timeout.
    for (bool wrongAlgorithm : { false, true }) {
        auto            transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(&session));
        auto            transport    = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
        TestApplication transfer(appPad, QStringLiteral("checksum-timeout"), J::Origin::Initiator,
                                 J::Origin::Responder);
        auto            file = testFile(4);
        file.addHash(Hash(Hash::Sha256));
        transfer.setFile(file);
        transfer.setAcceptFile(file);
        transfer.installTransport(transport);
        session.addContent(&transfer);
        QBuffer destination;
        destination.open(QIODevice::ReadWrite);
        QObject::connect(&transfer, &FT::Application::deviceRequested, &eventLoop,
                         [&](quint64, std::optional<quint64>) { transfer.setDevice(&destination, false); });
        transfer.prepare();
        transport->connection()->feed(QByteArrayLiteral("tail"));
        check(transfer.state() == J::State::Finishing, "Receiver did not wait for negotiated checksum");
        check(transport->connection()->isOpen(), "Receiver closed before checksum confirmation");
        auto timer = transfer.findChild<QTimer *>();
        check(timer && timer->isActive(), "Receiver did not arm checksum timeout");
        if (wrongAlgorithm) {
            receiveChecksum(
                ftPad, transfer.contentName(),
                Hash(Hash::Sha1, QCryptographicHash::hash(QByteArrayLiteral("tail"), QCryptographicHash::Sha1)));
        } else {
            check(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection), "Could not fire checksum timeout");
        }
        check(transfer.lastReason().condition() == J::Reason::FailedApplication,
              "Missing or wrong negotiated checksum was reported as success");
        receiveChecksum(
            ftPad, transfer.contentName(),
            Hash(Hash::Sha256, QCryptographicHash::hash(QByteArrayLiteral("tail"), QCryptographicHash::Sha256)));
        check(transfer.lastReason().condition() == J::Reason::FailedApplication,
              "Late checksum erased a terminal integrity failure");
        transfer.incomingRemove(J::Reason(J::Reason::Success));
        check(transfer.lastReason().condition() == J::Reason::FailedApplication,
              "Remote success erased the missing-checksum failure");
        check(!transport->connection()->isOpen() && !transfer.connection(),
              "Terminal failure retained the transport connection");
    }

    // A queued write is not delivery: keep the reliable transport alive until
    // the remote application confirms receipt, then release it immediately.
    {
        auto            transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(&session));
        auto            transport    = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
        TestApplication transfer(appPad, QStringLiteral("retain-until-receipt"), J::Origin::Initiator,
                                 J::Origin::Initiator);
        transfer.setFile(testFile(4));
        transfer.setAcceptFile(testFile(4));
        transfer.installTransport(transport);
        session.addContent(&transfer);
        QBuffer source;
        source.setData(QByteArrayLiteral("tail"));
        source.open(QIODevice::ReadOnly);
        QObject::connect(&transfer, &FT::Application::deviceRequested, &eventLoop,
                         [&](quint64, std::optional<quint64>) { transfer.setDevice(&source, false); });
        transfer.prepare();
        QCoreApplication::processEvents();
        check(transfer.state() == J::State::Finishing, "Sender did not enter receipt wait");
        check(transport->connection()->isOpen(), "Sender closed the transport before confirming delivery");
        receiveReceipt(ftPad, transfer.contentName());
        check(transfer.state() == J::State::Finished, "Receipt did not finish the transfer");
        check(!transport->connection()->isOpen(), "Confirmed transfer retained an open transport");
        check(!transfer.connection(), "Confirmed transfer retained its application connection");
    }

    // A matching receipt completes a sender once, but cannot finish a receiver
    // or replace an already terminal failure.
    {
        TestApplication sender(appPad, QStringLiteral("receipt-success"), J::Origin::Initiator, J::Origin::Initiator);
        sender.setFile(testFile(4));
        session.addContent(&sender);
        sender.setState(J::State::Finishing);
        int completions = 0;
        QObject::connect(&sender, &J::Application::stateChanged, &eventLoop,
                         [&](J::State state) { completions += state == J::State::Finished; });
        receiveReceipt(ftPad, sender.contentName());
        check(sender.state() == J::State::Finished && sender.lastReason().condition() == J::Reason::Success,
              "matching receipt did not complete sender successfully");
        receiveReceipt(ftPad, sender.contentName());
        check(completions == 1, "duplicate receipt completed sender more than once");

        TestApplication receiver(appPad, QStringLiteral("unexpected-receipt"), J::Origin::Initiator,
                                 J::Origin::Responder);
        receiver.setFile(testFile(4));
        session.addContent(&receiver);
        receiver.setState(J::State::Active);
        receiveReceipt(ftPad, receiver.contentName());
        check(receiver.state() == J::State::Active && !receiver.lastReason().isValid(),
              "incoming receipt prematurely completed a receiver");

        TestApplication failed(appPad, QStringLiteral("finished-failure"), J::Origin::Initiator, J::Origin::Initiator);
        failed.setFile(testFile(4));
        session.addContent(&failed);
        failed.setState(J::State::Active);
        failed.remove(J::Reason::FailedApplication, QStringLiteral("stream failed"));
        failed.setState(J::State::Finished);
        receiveReceipt(ftPad, failed.contentName());
        check(failed.lastReason().condition() == J::Reason::FailedApplication,
              "receipt overwrote finished transfer failure");
    }

    // IBB peer close can beat the queued bytesWritten callback for the final
    // block. All payload is queued, but success still requires a receipt.
    {
        auto            transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(&session));
        auto            transport    = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
        TestApplication transfer(appPad, QStringLiteral("close-before-write-notification"), J::Origin::Initiator,
                                 J::Origin::Initiator);
        transfer.setFile(testFile(4));
        transfer.setAcceptFile(testFile(4));
        transfer.installTransport(transport);
        session.addContent(&transfer);
        QBuffer source;
        source.setData(QByteArrayLiteral("tail"));
        source.open(QIODevice::ReadOnly);
        QObject::connect(&transfer, &FT::Application::deviceRequested, &eventLoop,
                         [&](quint64, std::optional<quint64>) { transfer.setDevice(&source, false); });
        transfer.prepare();
        check(transfer.state() == J::State::Active && source.atEnd(),
              "close-race fixture did not queue the final payload while active");
        emit transport->connection()->connectionClosed();
        check(transfer.state() == J::State::Finishing && !transfer.lastReason().isValid(),
              "peer close after final payload did not wait for receipt");
        receiveReceipt(ftPad, transfer.contentName());
        QCoreApplication::processEvents();
        check(transfer.state() == J::State::Finished && transfer.lastReason().condition() == J::Reason::Success,
              "peer close before write notification prevented successful receipt");
    }

    exerciseSessionTermination(client, false, false, J::Reason::Success);
    exerciseSessionTermination(client, false, true, J::Reason::Success);
    exerciseSessionTermination(client, true, false, J::Reason::Success);
    exerciseSessionTermination(client, false, false, J::Reason::Cancel);
    exerciseSessionTermination(client, false, false, J::Reason::Success, true);

    qInfo("Jingle FT finishing lifecycle regressions passed");
    return 0;
}
