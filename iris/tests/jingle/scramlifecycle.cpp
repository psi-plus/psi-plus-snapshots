// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtCrypto>
#include <cstdlib>
#include <iris/bytestream.h>
#include <iris/xmpp.h>
#include <iris/xmpp_clientstream.h>

using namespace XMPP;

static void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
        std::exit(1);
    }
}

class MemoryStream : public ByteStream {
public:
    void feed(const QByteArray &bytes)
    {
        appendRead(bytes);
        emit readyRead();
    }

protected:
    int tryWrite() override
    {
        const auto bytes = takeWrite();
        QTimer::singleShot(0, this, [this, size = bytes.size()] { emit bytesWritten(size); });
        return bytes.size();
    }
};

class MemoryConnector : public Connector {
public:
    MemoryStream socket;
    void         setOptHostPort(const QString &, quint16) override { }
    void         connectToServer(const QString &) override
    {
        socket.open(QIODevice::ReadWrite);
        emit connected();
    }
    ByteStream *stream() const override { return const_cast<MemoryStream *>(&socket); }
    void        done() override { socket.close(); }
    void        fail() { emit error(); }
};

static void checkNoSasl(ClientStream &stream)
{
    check(!stream.currentSASLContext(), "Unexpected SASL context");
    stream.setSCRAMStoredSaltedHash(QStringLiteral("cached-secret"));
    check(stream.getSCRAMStoredSaltedHash().isEmpty(), "Missing SASL context returned a SCRAM secret");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // PLAIN needs only Iris's in-tree SimpleSASL provider. Do not let unrelated
    // system plugins choose a different SASL implementation for this fixture.
    qputenv("QCA_NO_PLUGINS", "1");
    QCA::Initializer qca;
    MemoryConnector  connector;
    ClientStream     stream(&connector);
    checkNoSasl(stream); // Idle: previously both public methods crashed here.
    stream.setAllowPlain(ClientStream::AllowPlain);
    stream.setSaslMechanismProvider(QStringLiteral("PLAIN"), QStringLiteral("simplesasl"));
    QObject::connect(&stream, &ClientStream::warning, &stream, [&] { stream.continueAfterWarning(); });
    stream.connectToServer(Jid(QStringLiteral("alice@example.test/desktop")));
    checkNoSasl(stream); // Connecting, matching the stream targeted in the crash.

    bool       requested = false;
    QEventLoop loop;
    QObject::connect(&stream, &ClientStream::needAuthParams, &loop, [&](bool, bool, bool) {
        requested = true;
        loop.quit();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(3000);
    connector.socket.feed(
        QByteArrayLiteral("<stream:stream xmlns:stream='http://etherx.jabber.org/streams' xmlns='jabber:client' "
                          "from='example.test' version='1.0'><stream:features>"
                          "<mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'><mechanism>PLAIN</mechanism>"
                          "</mechanisms></stream:features>"));
    if (!requested)
        loop.exec();
    check(requested && stream.currentSASLContext(), "Real SASL credential request was not reached");
    stream.setSCRAMStoredSaltedHash(QStringLiteral("cached-secret"));
    check(stream.getSCRAMStoredSaltedHash() == QStringLiteral("cached-secret"),
          "SCRAM accessors lost the property on a live SASL context");
    connector.fail(); // Resets and deletes a previously live SASL object.
    checkNoSasl(stream);
    stream.connectToServer(Jid(QStringLiteral("alice@example.test/desktop")));
    checkNoSasl(stream); // A new attempt must not retain the old context.
    return 0;
}
