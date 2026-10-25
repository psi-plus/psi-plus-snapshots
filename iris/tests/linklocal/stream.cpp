/* Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <QCoreApplication>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <iris/xmpp_linklocalstream.h>
#include <iris/xmpp_stanza.h>
#include <memory>

using XMPP::Jid;
using XMPP::LinkLocalStream;
using XMPP::Stanza;

namespace {
struct Connection {
    QTcpServer                  server;
    QTcpSocket                  client;
    std::unique_ptr<QTcpSocket> accepted;
    bool                        open()
    {
        if (!server.listen(QHostAddress::LocalHost))
            return false;
        client.connectToHost(QHostAddress::LocalHost, server.serverPort());
        if (!client.waitForConnected(1000) || !server.waitForNewConnection(1000))
            return false;
        accepted.reset(server.nextPendingConnection());
        return bool(accepted);
    }
};
const Jid        Romeo("romeo@forza"), Juliet("juliet@pronto");
const QByteArray Header("<stream:stream xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams' "
                        "from='romeo@forza' to='juliet@pronto' version='1.0'>");
Stanza           message(LinkLocalStream &stream, const QString &body)
{
    auto stanza = stream.createStanza(Stanza::Message, stream.peerJid(), QStringLiteral("chat"));
    stanza.appendChild(stanza.createTextElement(stream.baseNS(), QStringLiteral("body"), body));
    return stanza;
}
}

class StreamTest : public QObject {
    Q_OBJECT
private slots:
    void exchangeAndClose()
    {
        Connection connection;
        QVERIFY(connection.open());
        LinkLocalStream recipient(connection.accepted.get(), Juliet, {}, LinkLocalStream::Direction::Recipient);
        LinkLocalStream initiator(&connection.client, Romeo, Juliet, LinkLocalStream::Direction::Initiator);
        QSignalSpy      incomingReady(&recipient, &LinkLocalStream::connected);
        QSignalSpy      outgoingReady(&initiator, &LinkLocalStream::connected);
        QSignalSpy      errors1(&recipient, &XMPP::Stream::error), errors2(&initiator, &XMPP::Stream::error);
        QVERIFY(incomingReady.isEmpty() && outgoingReady.isEmpty());
        QTRY_COMPARE(incomingReady.size(), 1);
        QTRY_COMPARE(outgoingReady.size(), 1);
        QCOMPARE(recipient.peerJid(), Romeo);
        initiator.write(message(initiator, QString::fromUtf8("Hello, Джульетта <&>")));
        QTRY_VERIFY(recipient.stanzaAvailable());
        auto received = recipient.read();
        QCOMPARE(received.from(), Romeo);
        QCOMPARE(received.to(), Juliet);
        QCOMPARE(received.element().firstChildElement("body").text(), QString::fromUtf8("Hello, Джульетта <&>"));
        auto iq = initiator.createStanza(Stanza::IQ, Juliet, QStringLiteral("get"), QStringLiteral("query-1"));
        iq.appendChild(
            iq.createElement(QStringLiteral("http://jabber.org/protocol/disco#info"), QStringLiteral("query")));
        initiator.write(iq);
        QTRY_VERIFY(recipient.stanzaAvailable());
        QCOMPARE(recipient.read().kind(), Stanza::IQ);
        recipient.write(recipient.createStanza(Stanza::IQ, Romeo, QStringLiteral("result"), iq.id()));
        QTRY_VERIFY(initiator.stanzaAvailable());
        QCOMPARE(initiator.read().id(), iq.id());
        // An in-flight reply is valid after the first side sent its close.
        recipient.write(message(recipient, QStringLiteral("Reply")));
        QSignalSpy done1(&initiator, &XMPP::Stream::delayedCloseFinished);
        QSignalSpy done2(&recipient, &XMPP::Stream::delayedCloseFinished);
        initiator.close();
        QTRY_VERIFY(initiator.stanzaAvailable());
        QCOMPARE(initiator.read().element().firstChildElement("body").text(), QStringLiteral("Reply"));
        QTRY_COMPARE(done1.size(), 1);
        QTRY_COMPARE(done2.size(), 1);
        QVERIFY(errors1.isEmpty());
        QVERIFY(errors2.isEmpty());
    }

    void rejectsInvalidInput_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<int>("condition");
        QTest::newRow("spoofed-stanza") << Header + "<message from='mallory@elsewhere'><body>fake</body></message>"
                                        << int(XMPP::Stream::InvalidFrom);
        QTest::newRow("wrong-recipient") << QByteArray(Header).replace("juliet@pronto", "mallory@other")
                                         << int(XMPP::Stream::InvalidFrom);
        QTest::newRow("presence") << Header + "<presence/>" << int(XMPP::Stream::PolicyViolation);
        QTest::newRow("wrong-namespace") << Header + "<message xmlns='evil'><body>fake</body></message>"
                                         << int(XMPP::Stream::PolicyViolation);
        QTest::newRow("dtd") << QByteArray("<!DOCTYPE stream [<!ENTITY attack 'expanded'>]>") + Header
                             << int(XMPP::Stream::InvalidXml);
        QTest::newRow("repeat-header") << Header + Header + "</stream:stream>" << int(XMPP::Stream::PolicyViolation);
        QTest::newRow("malformed") << Header + "<message></wrong>" << int(XMPP::Stream::InvalidXml);
    }
    void rejectsInvalidInput()
    {
        QFETCH(QByteArray, input);
        QFETCH(int, condition);
        Connection connection;
        QVERIFY(connection.open());
        LinkLocalStream recipient(connection.accepted.get(), Juliet, {}, LinkLocalStream::Direction::Recipient);
        QSignalSpy      errors(&recipient, &XMPP::Stream::error);
        // Exercise fragmented XML; the same Iris parser handles live streams.
        connection.client.write(input.left(19));
        QTest::qWait(5);
        connection.client.write(input.mid(19));
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(recipient.errorCondition(), condition);
        QVERIFY(!recipient.stanzaAvailable());
    }

    void requiredTlsIsNotSilentlyBypassed()
    {
        Connection connection;
        QVERIFY(connection.open());
        LinkLocalStream initiator(&connection.client, Romeo, Juliet, LinkLocalStream::Direction::Initiator);
        QSignalSpy      errors(&initiator, &XMPP::Stream::error);
        QSignalSpy      ready(&initiator, &LinkLocalStream::connected);
        const auto      reply = QByteArray(Header).replace("from='romeo@forza' to='juliet@pronto'",
                                                           "from='juliet@pronto' to='romeo@forza'");
        connection.accepted->write(reply);
        QTest::qWait(5);
        QVERIFY(ready.isEmpty());
        connection.accepted->write("<stream:features><starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'>"
                                   "<required/></starttls></stream:features>");
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(ready.isEmpty());
        QCOMPARE(initiator.errorCondition(), int(XMPP::Stream::PolicyViolation));
    }

    void legacyHeaderWithKnownPeer()
    {
        Connection connection;
        QVERIFY(connection.open());
        LinkLocalStream recipient(connection.accepted.get(), Juliet, Romeo, LinkLocalStream::Direction::Recipient);
        QSignalSpy      ready(&recipient, &LinkLocalStream::connected);
        connection.client.write(QByteArray(Header).replace(" version='1.0'", "").replace(" from='romeo@forza'", ""));
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(recipient.old());
        connection.client.write("<message><body>No TXT metadata needed</body></message>");
        QTRY_VERIFY(recipient.stanzaAvailable());
        QCOMPARE(recipient.read().from(), Romeo);
    }

    void oversizedIncompleteXmlIsBounded()
    {
        Connection connection;
        QVERIFY(connection.open());
        LinkLocalStream recipient(connection.accepted.get(), Juliet, {}, LinkLocalStream::Direction::Recipient);
        QSignalSpy      errors(&recipient, &XMPP::Stream::error);
        connection.client.write(Header + "<message><body>" + QByteArray(1024 * 1024 + 4096, 'x'));
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(recipient.errorCondition(), int(XMPP::Stream::ResourceConstraint));
    }

    void consumerMayDeleteFromConnected()
    {
        Connection connection;
        QVERIFY(connection.open());
        QPointer<LinkLocalStream> recipient
            = new LinkLocalStream(connection.accepted.get(), Juliet, {}, LinkLocalStream::Direction::Recipient);
        connect(recipient, &LinkLocalStream::connected, this, [recipient] { delete recipient; });
        connection.client.write(Header);
        QTRY_VERIFY(recipient.isNull());
    }
};
QTEST_GUILESS_MAIN(StreamTest)
#include "stream.moc"
