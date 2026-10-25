// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QUuid>
#include <QtTest>
#include <iris/xmpp_client.h>
#include <iris/xmpp_linklocalservice.h>
#include <iris/xmpp_linklocalstream.h>
#include <iris/xmpp_liveroster.h>
#include <iris/xmpp_message.h>
using namespace XMPP;

class ServiceTest : public QObject {
    Q_OBJECT
private slots:
    void discoveryClientMessagesPresenceAndGoodbye()
    {
        if (!LinkLocalService::isSupported())
            QSKIP("JDNS disabled in this build");
        const auto       suffix = QUuid::createUuid().toString(QUuid::Id128).left(8);
        const Jid        alice("alice@a-" + suffix), bob("bob@b-" + suffix);
        Client           a, b;
        LinkLocalService sa, sb;
        a.connectToStream(&sa, alice);
        b.connectToStream(&sb, bob);
        QList<Message> messagesA, messagesB;
        connect(&a, &Client::messageReceived, this, [&](const Message &m) { messagesA.append(m); });
        connect(&b, &Client::messageReceived, this, [&](const Message &m) { messagesB.append(m); });
        QString discoveredName;
        Status  discoveredStatus(Status::Offline);
        connect(&sa, &LinkLocalService::peerChanged, this, [&](const Jid &peer, const QString &name, const Status &s) {
            if (peer == bob) {
                discoveredName   = name;
                discoveredStatus = s;
            }
        });
        int failures = 0;
        connect(&sa, &Stream::error, this, [&](int) { ++failures; });
        connect(&sb, &Stream::error, this, [&](int) { ++failures; });
        QVERIFY(sa.start(alice, QHostAddress::LocalHost, "Alice"));
        QVERIFY(sb.start(bob, QHostAddress::LocalHost, QString::fromUtf8("Боб")));
        QTRY_VERIFY_WITH_TIMEOUT(sa.isOnline() && sb.isOnline(), 12000);
        QTRY_COMPARE_WITH_TIMEOUT(discoveredName, QString::fromUtf8("Боб"), 10000);
        QTRY_VERIFY(b.roster().find(alice) != b.roster().end());
        QTRY_VERIFY(a.roster().find(bob) != a.roster().end());
        a.setPresence(Status(Status::Online));
        b.setPresence(Status(Status::Away, "Lunch"));
        QTRY_COMPARE(discoveredStatus.type(), Status::Away);
        QTRY_COMPARE(discoveredStatus.status(), QString("Lunch"));
        sb.setDisplayName("Robert");
        QTRY_COMPARE(discoveredName, QString("Robert"));
        Message m;
        m.setTo(bob);
        m.setType(Message::Type::Chat);
        m.setId("first");
        m.setBody(QString::fromUtf8("Привет <&>"));
        a.sendMessage(m);
        QTRY_COMPARE_WITH_TIMEOUT(messagesB.size(), 1, 12000);
        QCOMPARE(messagesB.last().body(), m.body());
        QCOMPARE(messagesB.last().from(), alice);
        QCOMPARE(messagesB.last().to(), bob);
        m.setTo(alice);
        m.setId("reply");
        m.setBody("Reply");
        b.sendMessage(m);
        QTRY_COMPARE(messagesA.size(), 1);
        QCOMPARE(messagesA.last().from(), bob);
        m.setTo(bob);
        m.setId("correction");
        m.setReplaceId("first");
        m.setBody("Corrected");
        a.sendMessage(m);
        QTRY_COMPARE(messagesB.size(), 2);
        QCOMPARE(messagesB.last().replaceId(), QString("first"));
        QCOMPARE(messagesB.last().body(), QString("Corrected"));
        // A peer may close an idle XML stream while remaining online in mDNS.
        // Sending during its close handshake must open a new connection.
        const auto channels = sa.findChildren<LinkLocalStream *>();
        QVERIFY(!channels.isEmpty());
        for (auto *channel : channels)
            channel->close();
        m.setId("new-connection");
        a.sendMessage(m);
        QTRY_COMPARE(messagesB.size(), 3);
        Message receipt;
        receipt.setTo(bob);
        receipt.setType(Message::Type::Chat);
        receipt.setMessageReceipt(ReceiptReceived);
        receipt.setMessageReceiptId("first");
        a.sendMessage(receipt);
        QTRY_COMPARE(messagesB.size(), 4);
        QCOMPARE(messagesB.last().messageReceiptId(), QString("first"));
        Message reaction;
        reaction.setTo(bob);
        reaction.setType(Message::Type::Chat);
        reaction.setReactions({ "first", { QString::fromUtf8("👍") } });
        a.sendMessage(reaction);
        QTRY_COMPARE(messagesB.size(), 5);
        QCOMPARE(messagesB.last().reactions().targetId, QString("first"));
        Message retraction;
        retraction.setTo(bob);
        retraction.setType(Message::Type::Chat);
        retraction.setRetraction("first");
        a.sendMessage(retraction);
        QTRY_COMPARE(messagesB.size(), 6);
        QCOMPARE(messagesB.last().retraction(), QString("first"));
        int peerErrors = 0;
        connect(&sa, &LinkLocalService::peerError, this, [&](const Jid &, const QString &) { ++peerErrors; });
        m.setTo(Jid("nobody@missing"));
        a.sendMessage(m);
        QTRY_COMPARE(peerErrors, 1);
        QVERIFY(sa.isOnline() && sb.isOnline());
        sb.close();
        QTRY_COMPARE_WITH_TIMEOUT(discoveredStatus.type(), Status::Offline, 10000);
        QTRY_VERIFY(!sb.isOnline());
        QTRY_VERIFY_WITH_TIMEOUT(!b.isActive(), 10000);
        // Reattach the same Client. Its session tasks must not deliver twice.
        LinkLocalService again;
        b.connectToStream(&again, bob);
        QVERIFY(again.start(bob, QHostAddress::LocalHost, "Bob again"));
        QTRY_VERIFY_WITH_TIMEOUT(again.isOnline(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(discoveredName, QString("Bob again"), 10000);
        m.setTo(bob);
        m.setId("after-reconnect");
        m.setBody("Still one copy");
        m.setReplaceId(QString());
        a.sendMessage(m);
        QTRY_COMPARE_WITH_TIMEOUT(messagesB.size(), 7, 12000);
        QTest::qWait(150);
        QCOMPARE(messagesB.size(), 7);
        QCOMPARE(failures, 0);
        again.close();
        sa.close();
        QTRY_VERIFY(!again.isOnline() && !sa.isOnline());
        QTRY_VERIFY_WITH_TIMEOUT(!a.isActive() && !b.isActive(), 10000);
    }
    void failureIsDeferredAndUnknownPeerDoesNotDisconnect()
    {
        LinkLocalService invalid;
        bool             error = false;
        connect(&invalid, &Stream::error, this, [&](int) { error = true; });
        QVERIFY(!invalid.start(Jid("invalid"), QHostAddress::Any, "Invalid"));
        QVERIFY(!error);
        QTRY_VERIFY(error);
        QVERIFY(!invalid.errorText().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ServiceTest)
#include "service.moc"
