// SPDX-License-Identifier: LGPL-2.1-or-later
// MAM wire-level regression with the real Task dispatcher and a recording stream.
#include <QCoreApplication>
#include <QDomDocument>
#include <QSet>
#include <QSharedPointer>
#include <QtCrypto>
#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>
#include <iris/xmpp_features.h>
#include <iris/xmpp_mammanager.h>
#include <iris/xmpp_mamtask.h>
#include <iris/xmpp_tasks.h>
#include <iris/xmpp_xmlcommon.h>
using namespace XMPP;

static void require(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class OfflineConnector final : public Connector {
public:
    void        setOptHostPort(const QString &, quint16) override { }
    void        connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void        done() override { }
};
class Recorder final : public ClientStream {
public:
    explicit Recorder(Connector *c) : ClientStream(c) { }
    void               write(const Stanza &s) override { sent.append(s.element().cloneNode(true).toElement()); }
    QList<QDomElement> sent;
};
// Proves that a sibling task can still receive an IQ that does not
// belong to MAM. The root task dispatches children in creation order.
class IQProbe final : public Task {
public:
    explicit IQProbe(Task *parent) : Task(parent) { }

    bool take(const QDomElement &stanza) override
    {
        if (stanza.tagName() != QLatin1String("iq"))
            return false;
        ++received;
        return true;
    }

    int received = 0;
};
// A non-consuming subscriber runs before the MAM page subscriber and
// must not prevent the page from receiving the same validated Message.
class MAMObserver final : public JT_PushMessage::Subscriber {
public:
    JT_PushMessage::SubscriberResult messageEventResult(Message &message, int, bool nested) override
    {
        if (nested || !message.mamResult().isValid())
            return JT_PushMessage::SubscriberResult::NotHandled;
        ++observed;
        return JT_PushMessage::SubscriberResult::HandledContinue;
    }
    int observed = 0;
};
static QDomElement xml(const QString &text)
{
    // Qt5 requires the owning document to survive the Stanza/Message
    // processing of its nodes. A local QDomDocument leaves dangling nodes.
    static QList<QSharedPointer<QDomDocument>> documents;
    auto                                       doc = QSharedPointer<QDomDocument>::create();
    require(doc->setContent(text, true), "Malformed XML fixture");
    documents.append(doc);
    return doc->documentElement();
}
static QString qid(const QDomElement &iq)
{
    return iq.firstChildElement(QStringLiteral("query")).attribute(QStringLiteral("queryid"));
}
static QDomElement rsm(const QDomElement &iq)
{
    return iq.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("set"));
}
static bool hasField(const QDomElement &iq, const QString &var)
{
    auto fields = iq.elementsByTagName(QStringLiteral("field"));
    for (int i = 0; i < fields.count(); ++i)
        if (fields.at(i).toElement().attribute(QStringLiteral("var")) == var)
            return true;
    return false;
}
static void result(Client &c, const QDomElement &iq, const QString &from, const QString &id)
{
    const QString stanza
        = QStringLiteral("<message from='%1'><result xmlns='urn:xmpp:mam:2' queryid='%2' id='%3'>"
                         "<forwarded xmlns='urn:xmpp:forward:0'><delay xmlns='urn:xmpp:delay' "
                         "stamp='2026-10-16T12:00:00Z'/><message xmlns='jabber:client' "
                         "from='bob@example.test'><body>Hi</body></message></forwarded></result></message>")
              .arg(from, qid(iq), id);
    require(c.rootTask()->take(xml(stanza)), "MAM result not consumed");
}
static void fin(Client &c, const QDomElement &iq, const QString &from, const QString &first, const QString &last,
                bool complete, bool stable = true)
{
    const QString stanza = QStringLiteral("<iq from='%1' type='result' id='%2'><fin xmlns='urn:xmpp:mam:2' "
                                          "complete='%3' stable='%4'><set xmlns='http://jabber.org/protocol/rsm'>"
                                          "<first>%5</first><last>%6</last></set></fin></iq>")
                               .arg(from, iq.attribute(QStringLiteral("id")),
                                    complete ? QStringLiteral("true") : QStringLiteral("false"),
                                    stable ? QStringLiteral("true") : QStringLiteral("false"), first, last);
    require(c.rootTask()->take(xml(stanza)), "MAM fin not consumed");
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    // Shared XML helpers must inspect direct children only, without
    // mistaking a namespace prefix for the actual element name.
    const auto children = xml(QStringLiteral("<root><container><item/></container>"
                                             "<a:item xmlns:a='urn:example:first'/>"
                                             "<item xmlns='urn:example:second'/></root>"));
    require(firstChildElementByLocalName(children, QStringLiteral("item")).namespaceURI()
                == QLatin1String("urn:example:first"),
            "Direct-child local-name lookup skipped a prefixed element or descended recursively");
    require(childElementsByTagNameNS(children, QStringLiteral("urn:example:second"), QStringLiteral("item")).count()
                == 1,
            "Existing namespace-aware direct-child lookup was changed");

    OfflineConnector connector;
    Recorder         stream(&connector);
    Client           c;
    c.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(), QStringLiteral("desktop"));
    c.connectToServer(&stream, Jid(QStringLiteral("alice@example.test/desktop")));

    MAMObserver observer;
    c.pushMessage()->subscribeMessage(&observer, 0);
    int unhandledMamEnvelopes = 0;
    QObject::connect(c.pushMessage(), &JT_PushMessage::message, &c, [&](const Message &message) {
        if (message.mamResult().isValid())
            ++unhandledMamEnvelopes;
    });

    MAMTask baseline(c.rootTask());
    int     finished = 0;
    QObject::connect(&baseline, &Task::finished, &baseline, [&] { ++finished; });
    baseline.get(Jid(), QDateTime(), QDateTime(), false, 2, 0, true, true);
    baseline.go();
    require(!stream.sent.isEmpty(), "No initial MAM query");
    auto first = stream.sent.last();
    // A coordinator owns a child Task for the active page. IQ correlation
    // must stay in that child rather than the multi-page parent.
    require(baseline.children().size() == 1 && baseline.children().first()->inherits("XMPP::Task"),
            "MAM coordinator did not create a child page Task");
    auto *initialPage = baseline.children().first();
    require(baseline.timeout() == 0 && static_cast<Task *>(initialPage)->timeout() > 0,
            "MAM coordinator has a global default timeout or page timeout is disabled");
    // A caller can opt into an overall deadline without changing the
    // default per-page deadline for ordinary multi-page requests.
    MAMTask explicitDeadline(c.rootTask());
    explicitDeadline.setTimeout(15);
    require(explicitDeadline.timeout() == 15, "Explicit MAM deadline cannot be configured");
    require(first.attribute(QStringLiteral("to")).isEmpty(), "Own archive IQ has a to");
    require(!hasField(first, QStringLiteral("with")), "Unfiltered query has a with");
    require(!hasField(first, QStringLiteral("include-groupchat")), "Unadvertised field sent");
    require(first.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("flip-page")).isNull(),
            "Unadvertised flip-page sent");
    require(rsm(first).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("2"), "Wrong page size");
    require(!rsm(first).firstChildElement(QStringLiteral("before")).isNull(), "Missing last-page cursor");
    // Unrelated IQs must remain available to other Task handlers, even
    // if their IDs happen to overlap with the pending MAM request.
    {
        IQProbe    observer(c.rootTask());
        const auto id = first.attribute(QStringLiteral("id"));
        require(c.rootTask()->take(xml(QStringLiteral("<iq from='alice@example.test' type='result' id='unrelated-id'>"
                                                      "<query xmlns='jabber:iq:roster'/></iq>"))),
                "Unrelated IQ was not offered to the sibling task");
        require(observer.received == 1 && finished == 0, "MAM consumed a foreign IQ response with another id");

        // iqVerify() alone accepts a result from another own resource for
        // an own-archive request. MAM must decline rather than swallow it.
        const auto otherResource = QStringLiteral("<iq from='alice@example.test/phone' type='result' id='%1'>"
                                                  "<query xmlns='jabber:iq:roster'/></iq>")
                                       .arg(id);
        require(c.rootTask()->take(xml(otherResource)), "IQ from another own resource was not routed");
        require(observer.received == 2 && finished == 0, "MAM swallowed an IQ sent by another own resource");

        const auto serverResponse = QStringLiteral("<iq from='example.test' type='error' id='%1'>"
                                                   "<error type='cancel'><service-unavailable "
                                                   "xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/></error></iq>")
                                        .arg(id);
        require(c.rootTask()->take(xml(serverResponse)), "IQ from an unrelated service was not routed");
        require(observer.received == 3 && finished == 0, "MAM swallowed another service's IQ error");

        // XMPP IQ requests use a separate peer ID space; 'get'/'set'
        // with a matching id are not answers to our outgoing 'set'.
        const auto incomingRequest = QStringLiteral("<iq from='alice@example.test' type='set' id='%1'>"
                                                    "<query xmlns='urn:example:unrelated'/></iq>")
                                         .arg(id);
        require(!baseline.take(xml(incomingRequest)), "MAM treated an incoming IQ set as a query completion");

        require(baseline.archive().isEmpty() && finished == 0, "Unrelated IQs changed the active MAM query");
    }
    result(c, first, QStringLiteral("other@example.test"), QStringLiteral("fake"));
    result(c, first, QStringLiteral("alice@example.test/other-device"), QStringLiteral("fake2"));
    require(baseline.archive().isEmpty(), "Forged result was accepted");
    const auto invalidDelay
        = QStringLiteral("<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' queryid='%1' id='bad1'>"
                         "<forwarded xmlns='urn:xmpp:forward:0'><delay xmlns='urn:xmpp:delay' "
                         "stamp='not-a-date'/></forwarded></result></message>")
              .arg(qid(first));
    require(c.rootTask()->take(xml(invalidDelay)), "Invalid delay result not consumed");
    const auto invalidMessage
        = QStringLiteral("<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' queryid='%1' id='bad2'>"
                         "<forwarded xmlns='urn:xmpp:forward:0'><message xmlns='urn:invalid:message'/>"
                         "</forwarded></result></message>")
              .arg(qid(first));
    require(c.rootTask()->take(xml(invalidMessage)), "Invalid message result not consumed");
    require(baseline.archive().isEmpty(), "Malformed forwarded result was accepted");
    const int deliveredBeforeValid = unhandledMamEnvelopes;
    const int observedBeforeValid  = observer.observed;
    result(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m2"));
    result(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m3"));
    require(observer.observed == observedBeforeValid + 2, "MAM results bypassed the ordinary message subscriber chain");
    require(unhandledMamEnvelopes == deliveredBeforeValid,
            "Consumed active MAM page was also delivered as an ordinary message");
    require(finished == 0 && baseline.archive().size() == 2, "Page completed before fin");
    const int  sentBeforeSpoofedFin = stream.sent.size();
    const auto spoofedFin           = QStringLiteral("<iq from='alice@example.test/other-device' type='result' id='%1'>"
                                                               "<fin xmlns='urn:xmpp:mam:2' complete='true'/></iq>")
                                .arg(first.attribute(QStringLiteral("id")));
    require(!baseline.take(xml(spoofedFin)), "MAM swallowed a forged fin IQ instead of declining it");
    require(finished == 0 && stream.sent.size() == sentBeforeSpoofedFin,
            "Forged IQ from another own resource completed the archive query");
    fin(c, first, QStringLiteral("alice@example.test"), QStringLiteral("m2"), QStringLiteral("m3"), false, false);
    auto second = stream.sent.last();
    require(baseline.children().size() >= 2 && baseline.children().last() != initialPage
                && baseline.children().last()->inherits("XMPP::Task"),
            "Next page reused the previous child Task");
    // The retired page still exists until deleteLater(), but it must not
    // consume a late result or IQ from its old query.
    const auto stale = QStringLiteral("<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' "
                                      "queryid='%1' id='late-old-page'><forwarded xmlns='urn:xmpp:forward:0'/>"
                                      "</result></message>")
                           .arg(qid(first));
    require(!baseline.take(xml(stale)), "Retired page consumed a late MAM result");
    require(!baseline.take(xml(QStringLiteral("<iq from='alice@example.test' type='result' id='%1'>"
                                              "<fin xmlns='urn:xmpp:mam:2' complete='true'/></iq>")
                                   .arg(first.attribute(QStringLiteral("id"))))),
            "Retired page consumed a late MAM IQ");
    require(baseline.archive().size() == 2, "Late page result changed coordinator results");
    require(qid(first) != qid(second), "Query ID was reused");
    require(rsm(second).firstChildElement(QStringLiteral("before")).text() == QStringLiteral("m2"),
            "Wrong backward cursor");
    result(c, second, QStringLiteral("alice@example.test"), QStringLiteral("m1"));
    fin(c, second, QStringLiteral("alice@example.test"), QStringLiteral("m1"), QStringLiteral("m1"), true);
    require(finished == 1 && baseline.success() && baseline.archive().size() == 3, "Pagination failed");
    require(baseline.isComplete() && !baseline.isStable(), "Fin flags not retained");

    // Query configuration is replaceable before go(), and the choice of
    // time vs ID range is exclusive. The previous filter must not leak.
    MAMTask reconfiguredTime(c.rootTask());
    reconfiguredTime.get(Jid(), QStringLiteral("obsolete"), QStringLiteral("old-end"), false, 2, 0, false, false);
    const auto begin = QDateTime::fromString(QStringLiteral("2026-10-15T12:00:00Z"), Qt::ISODate);
    const auto end   = QDateTime::fromString(QStringLiteral("2026-10-16T12:00:00Z"), Qt::ISODate);
    reconfiguredTime.get(Jid(), begin, end, false, 3, 0, false, false);
    reconfiguredTime.go();
    const auto timeIq = stream.sent.last();
    require(hasField(timeIq, QStringLiteral("start")) && hasField(timeIq, QStringLiteral("end"))
                && !hasField(timeIq, QStringLiteral("after-id")) && !hasField(timeIq, QStringLiteral("before-id"))
                && rsm(timeIq).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("3"),
            "Reconfigured time request leaked stale ID filters or page size");
    fin(c, timeIq, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(reconfiguredTime.success(), "Reconfigured time query failed");

    MAMTask  reconfiguredId(c.rootTask());
    Features extendedForReconfiguration;
    extendedForReconfiguration.addFeature(QStringLiteral("urn:xmpp:mam:2#extended"));
    reconfiguredId.setArchiveFeatures(extendedForReconfiguration);
    reconfiguredId.get(Jid(), begin, end, false, 3, 0, false, false);
    reconfiguredId.get(Jid(), QStringLiteral("new-start"), QStringLiteral("new-end"), false, 4, 0, false, false);
    reconfiguredId.go();
    const auto idIq = stream.sent.last();
    require(hasField(idIq, QStringLiteral("after-id")) && hasField(idIq, QStringLiteral("before-id"))
                && !hasField(idIq, QStringLiteral("start")) && !hasField(idIq, QStringLiteral("end"))
                && rsm(idIq).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("4"),
            "Reconfigured ID request leaked stale time filters or page size");
    fin(c, idIq, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(reconfiguredId.success(), "Reconfigured ID query failed");

    // Calling get() during a running request cannot silently wipe buffered
    // results or replace its active query parameters.
    MAMTask immutableWhileRunning(c.rootTask());
    immutableWhileRunning.get(Jid(), begin, end, false, 2, 0, false, false);
    immutableWhileRunning.go();
    const auto immutableIq = stream.sent.last();
    result(c, immutableIq, QStringLiteral("alice@example.test"), QStringLiteral("still-here"));
    immutableWhileRunning.get(Jid(), QStringLiteral("other-start"), QStringLiteral("other-end"), false, 2, 0, false,
                              true);
    require(immutableWhileRunning.archive().size() == 1, "Configuring a running task erased buffered results");
    fin(c, immutableIq, QStringLiteral("alice@example.test"), QStringLiteral("still-here"),
        QStringLiteral("still-here"), true);
    require(immutableWhileRunning.success() && immutableWhileRunning.archive().size() == 1,
            "Configuration changed the in-flight task or lost its archive");

    // XEP-0313 tombstones retain their archive ID even without a message.
    // Missing delay and message are valid; malformed present elements are not.
    MAMTask optionalForwarding(c.rootTask());
    optionalForwarding.get(Jid(), QDateTime(), QDateTime(), false, 10, 0, false, false);
    optionalForwarding.go();
    const auto optionalIq   = stream.sent.last();
    const auto sendOptional = [&](const QString &id, const QString &payload) {
        const auto stanza
            = QStringLiteral("<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' queryid='%1' id='%2'>"
                             "<forwarded xmlns='urn:xmpp:forward:0'>%3</forwarded></result></message>")
                  .arg(qid(optionalIq))
                  .arg(id)
                  .arg(payload);
        require(c.rootTask()->take(xml(stanza)), "Optional forwarded result not consumed");
    };
    sendOptional(QStringLiteral("deleted"),
                 QStringLiteral("<delay xmlns='urn:xmpp:delay' stamp='2026-10-16T12:00:00Z'/>"));
    sendOptional(QStringLiteral("undelayed"), QStringLiteral("<message xmlns='jabber:client'/>"));
    sendOptional(QStringLiteral("empty"), QString());
    require(optionalForwarding.archive().size() == 3, "Valid optional forwarding was rejected");
    require(optionalForwarding.archive().at(0).attribute(QStringLiteral("id")) == QStringLiteral("deleted"),
            "Tombstone archive UID was lost");
    require(optionalForwarding.archive()
                .at(0)
                .firstChildElement(QStringLiteral("forwarded"))
                .firstChildElement(QStringLiteral("message"))
                .isNull(),
            "Tombstone acquired a fabricated message");
    fin(c, optionalIq, QStringLiteral("alice@example.test"), QStringLiteral("deleted"), QStringLiteral("empty"), true);
    require(optionalForwarding.success(), "Optional forwarding query failed");

    // Namespace prefixes do not change XML element identity (XEP-0313).
    MAMTask prefixed(c.rootTask());
    prefixed.get(Jid(), QDateTime(), QDateTime(), false, 10, 0, false, false);
    prefixed.go();
    const auto prefixedIq     = stream.sent.last();
    const auto prefixedResult = QStringLiteral("<message from='alice@example.test' xmlns:mam='urn:xmpp:mam:2' "
                                               "xmlns:f='urn:xmpp:forward:0' xmlns:d='urn:xmpp:delay' "
                                               "xmlns:c='jabber:client'><mam:result queryid='%1' id='prefixed-uid'>"
                                               "<f:forwarded><d:delay stamp='2026-10-16T12:00:00Z'/>"
                                               "<c:message from='bob@example.test'><c:body>Hi</c:body></c:message>"
                                               "</f:forwarded></mam:result></message>")
                                    .arg(qid(prefixedIq));
    require(c.rootTask()->take(xml(prefixedResult)), "Prefixed MAM result not consumed");
    require(prefixed.archive().size() == 1
                && prefixed.archive().first().attribute(QStringLiteral("id")) == QStringLiteral("prefixed-uid"),
            "Valid prefixed MAM result was lost");
    const auto prefixedFin = QStringLiteral("<iq from='alice@example.test' type='result' id='%1' "
                                            "xmlns:mam='urn:xmpp:mam:2'><mam:fin complete='true'/></iq>")
                                 .arg(prefixedIq.attribute(QStringLiteral("id")));
    require(c.rootTask()->take(xml(prefixedFin)), "Prefixed MAM fin not consumed");
    require(prefixed.success(), "Valid prefixed MAM fin failed");

    MAMTask prefixedPaged(c.rootTask());
    prefixedPaged.get(Jid(), QDateTime(), QDateTime(), false, 2, 0, false, false);
    prefixedPaged.go();
    const auto firstPrefixedPage = stream.sent.last();
    const auto finWithPrefixedRsm
        = QStringLiteral("<iq from='alice@example.test' type='result' id='%1' "
                         "xmlns:mam='urn:xmpp:mam:2' xmlns:r='http://jabber.org/protocol/rsm'>"
                         "<mam:fin complete='false'><r:set><r:first>oldest</r:first>"
                         "<r:last>newest</r:last></r:set></mam:fin></iq>")
              .arg(firstPrefixedPage.attribute(QStringLiteral("id")));
    require(c.rootTask()->take(xml(finWithPrefixedRsm)), "Prefixed RSM fin not consumed");
    const auto secondPrefixedPage = stream.sent.last();
    require(secondPrefixedPage.attribute(QStringLiteral("id")) != firstPrefixedPage.attribute(QStringLiteral("id"))
                && rsm(secondPrefixedPage).firstChildElement(QStringLiteral("after")).text()
                    == QStringLiteral("newest"),
            "Prefixed RSM cursor did not advance pagination");
    fin(c, secondPrefixedPage, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(prefixedPaged.success(), "Prefixed RSM pagination did not finish");

    MAMTask   muc(c.rootTask());
    const Jid room(QStringLiteral("room@conference.example.test"));
    muc.setArchiveJid(room);
    muc.get(Jid(), QDateTime(), QDateTime(), true, 10, 0, true, false);
    muc.go();
    auto roomIq = stream.sent.last();
    require(roomIq.attribute(QStringLiteral("to")) == room.full(), "MUC query not addressed to room");
    // The archive identity is frozen at go(): changing it mid-query must
    // not redirect result authentication to a different MUC room.
    muc.setArchiveJid(Jid(QStringLiteral("other@conference.example.test")));
    require(!hasField(roomIq, QStringLiteral("with")), "MUC room used as with filter");
    result(c, roomIq, QStringLiteral("other@example.test"), QStringLiteral("fake"));
    result(c, roomIq, QStringLiteral("room@conference.example.test/occupant"), QStringLiteral("fake2"));
    require(muc.archive().isEmpty(), "Forged room result accepted");
    result(c, roomIq, room.full(), QStringLiteral("room1"));
    fin(c, roomIq, room.full(), QStringLiteral("room1"), QStringLiteral("room1"), true);
    require(muc.success() && muc.archive().size() == 1, "Room archive failed");

    MAMTask id(c.rootTask());
    id.get(Jid(), QStringLiteral("m1"), QStringLiteral("m5"), false, 10, 0, false, false);
    auto count = stream.sent.size();
    id.go();
    require(!id.success() && stream.sent.size() == count, "Unadvertised extended filter sent");
    MAMTask  extended(c.rootTask());
    Features features;
    features.addFeature(QStringLiteral("urn:xmpp:mam:2#extended"));
    extended.setArchiveFeatures(features);
    extended.get(Jid(), QStringLiteral("m1"), QString(), false, 10, 0, false, false);
    extended.go();
    require(hasField(stream.sent.last(), QStringLiteral("after-id")), "Extended ID filter missing");
    fin(c, stream.sent.last(), QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(extended.success(), "Empty extended query did not finish");

    // Baseline RSM supports one-sided ID cursors without #extended.
    MAMTask fallback(c.rootTask());
    fallback.get(Jid(), QStringLiteral("m1"), QString(), false, 10, 0, false, false);
    fallback.go();
    auto after = stream.sent.last();
    require(!hasField(after, QStringLiteral("after-id")), "Fallback sent an extended field");
    require(rsm(after).firstChildElement(QStringLiteral("after")).text() == QStringLiteral("m1"),
            "Baseline forward ID cursor missing");
    fin(c, after, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(fallback.success(), "Baseline forward ID query failed");

    MAMTask before(c.rootTask());
    before.get(Jid(), QString(), QStringLiteral("m5"), false, 10, 0, false, true);
    before.go();
    auto prior = stream.sent.last();
    require(!hasField(prior, QStringLiteral("before-id")), "Fallback sent an extended field");
    require(rsm(prior).firstChildElement(QStringLiteral("before")).text() == QStringLiteral("m5"),
            "Baseline backward ID cursor missing");
    fin(c, prior, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(before.success(), "Baseline backward ID query failed");

    // The returned task is configurable before go(): reverse after-id
    // requires the archive's disco-advertised MAM #extended feature.
    MAMManager manager(&c);
    auto      *unsupportedLatest      = manager.getLatestMessagesFromArchive(Jid(), QStringLiteral("m1"), false, 10);
    const auto countBeforeUnsupported = stream.sent.size();
    unsupportedLatest->go();
    require(!unsupportedLatest->success() && stream.sent.size() == countBeforeUnsupported,
            "Reverse after-id silently used unsupported baseline RSM");

    auto *latest = manager.getLatestMessagesFromArchive(Jid(), QStringLiteral("m1"), false, 10);
    latest->setArchiveFeatures(features);
    latest->go();
    const auto latestIq = stream.sent.last();
    require(hasField(latestIq, QStringLiteral("after-id")), "Manager dropped extended after-id");
    require(!rsm(latestIq).firstChildElement(QStringLiteral("before")).isNull(), "Manager lost reverse pagination");
    fin(c, latestIq, QStringLiteral("alice@example.test"), QString(), QString(), true);
    require(latest->success(), "Manager extended reverse query did not finish");

    // XEP-0059 permits a scrollbar jump by RSM index if the server
    // advertised a count. This is one page and does not change the normal
    // opaque-UID pagination mode.
    auto *indexed = manager.getArchiveAtIndex(Jid(), 371, 10, false);
    indexed->go();
    const auto indexedIq = stream.sent.last();
    require(rsm(indexedIq).firstChildElement(QStringLiteral("index")).text() == QStringLiteral("371"),
            "MAM indexed page sent no position");
    require(rsm(indexedIq).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("10"),
            "MAM indexed page size changed");
    result(c, indexedIq, QStringLiteral("alice@example.test"), QStringLiteral("indexed-371"));
    const auto indexedFin = QStringLiteral("<iq from='alice@example.test' type='result' id='%1'>"
                                           "<fin xmlns='urn:xmpp:mam:2' complete='false'>"
                                           "<set xmlns='http://jabber.org/protocol/rsm'>"
                                           "<first index='371'>indexed-371</first>"
                                           "<last>indexed-371</last><count>800</count>"
                                           "</set></fin></iq>")
                                .arg(indexedIq.attribute(QStringLiteral("id")));
    require(c.rootTask()->take(xml(indexedFin)), "Indexed MAM result not consumed");
    require(indexed->success() && indexed->archive().size() == 1 && indexed->resultCount() == 800
                && indexed->firstResultIndex() == 371,
            "RSM count / first index lost on the way to MAMTask");
    require(baseline.resultCount() == -1 && baseline.firstResultIndex() == -1,
            "Missing RSM metadata must remain unknown, not zero");

    // An indexed retrieval is ONE page even for short/empty non-final
    // responses. Optional RSM metadata must not turn into a made-up zero.
    struct IndexedCase {
        int         index, entries, count, first;
        const char *metadata;
    };
    const IndexedCase indexedCases[] = {
        { 0, 3, 800, 0, "<first index='0'>i-0</first><last>i-2</last><count>800</count>" },
        { 400, 2, -1, 400, "<first index='400'>i-0</first><last>i-1</last>" },
        { 798, 2, 800, 798, "<first index='798'>i-0</first><last>i-1</last><count>800</count>" },
        { 800, 0, 800, -1, "<count>800</count>" },
        { 0, 0, 0, -1, "<count>0</count>" },
        { 20, 1, -1, -1, "<first>i-0</first><last>i-0</last>" },
        { 20, 1, -1, -1, "<first index='bad'>i-0</first><count>-4</count>" },
        { 20, 1, -1, -1, "<first index='-1'>i-0</first><count>99999999999999999</count>" },
    };
    QSet<QString> indexedIqIds, indexedQueryIds;
    indexedIqIds.insert(indexedIq.attribute(QStringLiteral("id")));
    indexedQueryIds.insert(qid(indexedIq));
    for (const auto &test : indexedCases) {
        auto *task     = manager.getArchiveAtIndex(Jid(), test.index, 10, false);
        int   finishes = 0;
        QObject::connect(task, &Task::finished, task, [&finishes] { ++finishes; });
        const int sentBefore = stream.sent.size();
        task->go();
        const auto request = stream.sent.last();
        require(!indexedIqIds.contains(request.attribute(QStringLiteral("id")))
                    && !indexedQueryIds.contains(qid(request)),
                "Indexed requests reused IQ/queryid");
        indexedIqIds.insert(request.attribute(QStringLiteral("id")));
        indexedQueryIds.insert(qid(request));
        require(rsm(request).firstChildElement(QStringLiteral("before")).isNull()
                    && rsm(request).firstChildElement(QStringLiteral("after")).isNull(),
                "Indexed request accidentally retained an opaque cursor");
        for (int i = 0; i < test.entries; ++i)
            result(c, request, QStringLiteral("alice@example.test"), QStringLiteral("i-%1").arg(i));
        require(
            c.rootTask()->take(xml(
                QStringLiteral(
                    "<iq from='alice@example.test' type='result' id='%1'><fin xmlns='urn:xmpp:mam:2' "
                    "complete='false' stable='false'><set xmlns='http://jabber.org/protocol/rsm'>%2</set></fin></iq>")
                    .arg(request.attribute(QStringLiteral("id")), QString::fromLatin1(test.metadata)))),
            "Indexed fin not consumed");
        require(task->success() && finishes == 1 && task->archive().size() == test.entries
                    && task->resultCount() == test.count && task->firstResultIndex() == test.first && !task->isStable()
                    && !task->isComplete(),
                "Indexed page metadata/completion changed");
        QCoreApplication::processEvents();
        require(stream.sent.size() == sentBefore + 1, "Short indexed page continued paging");
        QObject::disconnect(task, nullptr, task, nullptr);
        task->deleteLater();
    }
    for (const auto &condition : { QStringLiteral("feature-not-implemented"), QStringLiteral("bad-request") }) {
        auto     *task       = manager.getArchiveAtIndex(Jid(), 100, 10, false);
        const int sentBefore = stream.sent.size();
        task->go();
        const auto request = stream.sent.last();
        require(c.rootTask()->take(
                    xml(QStringLiteral("<iq from='alice@example.test' type='error' id='%1'><error type='cancel'>"
                                       "<%2 xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/></error></iq>")
                            .arg(request.attribute(QStringLiteral("id")), condition))),
                "Indexed error not consumed");
        require(!task->success() && !task->statusString().isEmpty() && stream.sent.size() == sentBefore + 1,
                "Unsupported index started a second query or lost the error");
        task->deleteLater();
    }
    auto     *negativeIndex  = manager.getArchiveAtIndex(Jid(), -1, 10, false);
    const int beforeNegative = stream.sent.size();
    negativeIndex->go();
    require(!negativeIndex->success() && stream.sent.size() == beforeNegative,
            "Negative index silently fetched a different page");
    negativeIndex->deleteLater();

    auto *ignoredIndex = manager.getArchiveAtIndex(Jid(), 100, 10, false);
    ignoredIndex->go();
    const auto ignoredRequest = stream.sent.last();
    result(c, ignoredRequest, QStringLiteral("alice@example.test"), QStringLiteral("wrong-position"));
    require(c.rootTask()->take(
                xml(QStringLiteral("<iq from='alice@example.test' type='result' id='%1'><fin xmlns='urn:xmpp:mam:2'>"
                                   "<set xmlns='http://jabber.org/protocol/rsm'><first index='0'>wrong-position</first>"
                                   "<count>800</count></set></fin></iq>")
                        .arg(ignoredRequest.attribute(QStringLiteral("id"))))),
            "Ignored index response not consumed");
    require(!ignoredIndex->success(), "Server ignored the index but seek succeeded");
    ignoredIndex->deleteLater();

    MAMTask partiallyIndexed(c.rootTask());
    partiallyIndexed.get(Jid(), QDateTime(), QDateTime(), false, 1, 2, false, false);
    partiallyIndexed.go();
    const auto metadataPage1 = stream.sent.last();
    result(c, metadataPage1, QStringLiteral("alice@example.test"), QStringLiteral("meta-first"));
    fin(c, metadataPage1, QStringLiteral("alice@example.test"), QStringLiteral("meta-first"),
        QStringLiteral("meta-first"), false);
    const auto metadataPage2 = stream.sent.last();
    result(c, metadataPage2, QStringLiteral("alice@example.test"), QStringLiteral("meta-second"));
    require(c.rootTask()->take(xml(
                QStringLiteral(
                    "<iq from='alice@example.test' type='result' id='%1'><fin xmlns='urn:xmpp:mam:2' complete='true'>"
                    "<set xmlns='http://jabber.org/protocol/rsm'><first index='1'>meta-second</first>"
                    "<last>meta-second</last><count>2</count></set></fin></iq>")
                    .arg(metadataPage2.attribute(QStringLiteral("id"))))),
            "Second metadata page not consumed");
    require(partiallyIndexed.success() && partiallyIndexed.firstResultIndex() == -1
                && partiallyIndexed.resultCount() == 2,
            "A later page index was incorrectly attributed to the first result");

    // Bounded pagination must cap the final RSM page without claiming the
    // archive is exhausted. A streaming consumer must not retain results.
    MAMTask  bounded(c.rootTask());
    Features all;
    all.addFeature(QStringLiteral("urn:xmpp:mam:2#extended"));
    all.addFeature(QStringLiteral("urn:xmpp:mam:2#groupchat-field"));
    bounded.setArchiveFeatures(all);
    bounded.setCollectResults(false);
    int delivered = 0;
    QObject::connect(&bounded, &MAMTask::resultReceived, &bounded, [&](const QDomElement &entry) {
        require(!entry.attribute(QStringLiteral("id")).isEmpty(), "Streamed result has no archive id");
        ++delivered;
    });
    bounded.get(Jid(), QDateTime(), QDateTime(), false, 2, 3, true, false);
    bounded.go();
    auto page1 = stream.sent.last();
    require(hasField(page1, QStringLiteral("include-groupchat")), "Advertised groupchat field missing");
    require(!page1.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("flip-page")).isNull(),
            "Advertised flip-page missing");
    result(c, page1, QStringLiteral("alice@example.test"), QStringLiteral("b1"));
    result(c, page1, QStringLiteral("alice@example.test"), QStringLiteral("b2"));
    fin(c, page1, QStringLiteral("alice@example.test"), QStringLiteral("b1"), QStringLiteral("b2"), false);
    auto page2 = stream.sent.last();
    require(rsm(page2).firstChildElement(QStringLiteral("max")).text() == QStringLiteral("1"),
            "Final RSM page exceeds remaining cap");
    require(rsm(page2).firstChildElement(QStringLiteral("after")).text() == QStringLiteral("b2"),
            "Wrong forward cursor");
    result(c, page2, QStringLiteral("alice@example.test"), QStringLiteral("b3"));
    fin(c, page2, QStringLiteral("alice@example.test"), QStringLiteral("b3"), QStringLiteral("b3"), false);
    require(bounded.success() && !bounded.isComplete() && delivered == 3,
            "Bounded streaming query did not terminate correctly");
    require(bounded.archive().isEmpty(), "Streaming query retained buffered results");
    const int deliveredBeforeUnknown = unhandledMamEnvelopes;
    require(c.rootTask()->take(xml(QStringLiteral("<message from='alice@example.test'><result xmlns='urn:xmpp:mam:2' "
                                                  "queryid='no-open-query' id='late'/></message>"))),
            "Unknown MAM result did not traverse the normal message pipeline");
    require(unhandledMamEnvelopes == deliveredBeforeUnknown + 1,
            "A non-matching MAM page suppressed ordinary message dispatch");

    MAMTask failed(c.rootTask());
    failed.get(Jid(), QDateTime(), QDateTime(), false);
    failed.go();
    const auto errorQuery = stream.sent.last();
    const auto errorIq    = QStringLiteral("<iq from='alice@example.test' type='error' id='%1'><error type='cancel'>"
                                              "<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
                                              "</error></iq>")
                             .arg(errorQuery.attribute(QStringLiteral("id")));
    require(c.rootTask()->take(xml(errorIq)), "MAM IQ error was not consumed");
    require(!failed.success() && !failed.statusString().isEmpty(), "MAM IQ error was not propagated");

    // A correctly correlated result from the archive with no MAM <fin/>
    // is a malformed reply and must fail explicitly, not wait for timeout.
    MAMTask missingFin(c.rootTask());
    missingFin.get(Jid(), QDateTime(), QDateTime(), false);
    missingFin.go();
    const auto noFinIq          = stream.sent.last();
    const auto unrelatedPayload = QStringLiteral("<iq from='alice@example.test' type='result' id='%1'>"
                                                 "<query xmlns='jabber:iq:roster'/></iq>")
                                      .arg(noFinIq.attribute(QStringLiteral("id")));
    require(c.rootTask()->take(xml(unrelatedPayload)), "Correlated malformed MAM result was not handled");
    require(!missingFin.success() && !missingFin.statusString().isEmpty(),
            "Malformed correlated MAM result did not report a protocol error");

    MAMTask loop(c.rootTask());
    loop.get(Jid(), QDateTime(), QDateTime(), false, 2, 0, false, false);
    loop.go();
    const auto loopPage1 = stream.sent.last();
    fin(c, loopPage1, QStringLiteral("alice@example.test"), QStringLiteral("same"), QStringLiteral("same"), false);
    const auto loopPage2 = stream.sent.last();
    fin(c, loopPage2, QStringLiteral("alice@example.test"), QStringLiteral("same"), QStringLiteral("same"), false);
    require(!loop.success() && !loop.statusString().isEmpty(), "Repeated cursor caused an infinite loop");
    c.pushMessage()->unsubscribeMessage(&observer);
    return 0;
}
