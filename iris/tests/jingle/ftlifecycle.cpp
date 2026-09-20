// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/jingle-ft.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QPointer>

#include <cstring>

using namespace XMPP;
namespace J = XMPP::Jingle;
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

    qint64 bytesAvailable() const override
    {
        return incoming_.size() + J::Connection::bytesAvailable();
    }

    void feed(const QByteArray &data)
    {
        incoming_ += data;
        emit readyRead();
    }

    void closeDuringNextRead() { closeDuringRead_ = true; }

protected:
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
    J::Session *session() const override { return session_; }
    QString ns() const override { return QStringLiteral("urn:iris:test:ft"); }
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
    void start() override { setState(J::State::Active); }
    bool update(const QDomElement &) override { return true; }
    bool hasUpdates() const override { return false; }
    J::OutgoingTransportInfoUpdate takeOutgoingUpdate(bool = false) override { return {}; }
    bool isValid() const override { return true; }
    J::TransportFeatures features() const override { return connection_->features(); }
    J::Connection::Ptr addChannel(J::TransportFeatures, const QString &, int = -1) override { return connection_; }
    QList<J::Connection::Ptr> channels() const override { return { connection_ }; }

    QSharedPointer<TestConnection> connection() const { return connection_; }

private:
    QSharedPointer<TestConnection> connection_;
};

class TestApplication final : public FT::Application {
public:
    using FT::Application::Application;
    void installTransport(const QSharedPointer<J::Transport> &transport) { _transport = transport; }
};

static FT::File testFile(quint64 size)
{
    FT::File file;
    file.setName(QStringLiteral("tail.bin"));
    file.setSize(size);
    return file;
}

int main(int argc, char **argv)
{
    QCoreApplication eventLoop(argc, argv);
    Client client;
    J::Session session(client.jingleManager(), Jid(QStringLiteral("peer@example.test/device")), J::Origin::Initiator);

    auto *rawPad = client.jingleManager()->applicationPad(&session, FT::NS);
    auto *ftPad = dynamic_cast<FT::Pad *>(rawPad);
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
        check(update.reason.condition() == J::Reason::Condition::MediaError,
              "fatal Finishing error lost its reason");
        check(transfer.lastReason().condition() == J::Reason::Condition::MediaError,
              "fatal Finishing error was not made irreversible");
    }

    // stateChanged is a synchronous reentrancy boundary. Deleting the
    // application when a known-size receiver reaches Finishing must not leave
    // beginPayloadFinishing() touching its destroyed Private afterwards.
    {
        auto transportPad = J::TransportManagerPad::Ptr(new TestTransportPad(&session));
        auto transport = QSharedPointer<TestTransport>::create(transportPad, J::Origin::Initiator);
        auto *transfer = new TestApplication(appPad, QStringLiteral("reentrant"),
                                             J::Origin::Initiator, J::Origin::Responder);
        transfer->setFile(testFile(4));
        transfer->setAcceptFile(testFile(4));
        transfer->installTransport(transport);

        QBuffer sink;
        sink.open(QIODevice::WriteOnly);
        QObject::connect(transfer, &FT::Application::deviceRequested, &eventLoop,
                         [transfer, &sink](quint64, std::optional<quint64>) {
                             transfer->setDevice(&sink, false);
                         });

        QPointer<TestApplication> guard(transfer);
        int prematureUpdates = 0;
        QObject::connect(transfer, &J::Application::updated, &eventLoop, [&]() { ++prematureUpdates; });
        QObject::connect(transfer, &J::Application::stateChanged, &eventLoop,
                         [transfer](J::State state) {
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

    qInfo("Jingle FT finishing lifecycle regressions passed");
    return 0;
}
