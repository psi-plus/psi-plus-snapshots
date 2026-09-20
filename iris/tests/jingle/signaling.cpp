// Focused signaling regressions. No XMPP server or media engine is required.
#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <iris/dtls.h>
#include <iris/jingle-application.h>
#include <iris/xmpp_client.h>
#include <qca.h>

// Exercise the same parser entry point that JTPush calls, without network I/O.
#define private public
#include <iris/jingle-session.h>
#undef private

using namespace XMPP;
using namespace XMPP::Jingle;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

class TestPad : public ApplicationManagerPad {
public:
    explicit TestPad(Session *session) : s(session) { }
    Session            *session() const override { return s; }
    QString             ns() const override { return QStringLiteral("urn:iris:test:application"); }
    ApplicationManager *manager() const override { return nullptr; }
    QString             generateContentName(Origin) override { return QStringLiteral("test"); }

private:
    Session *s;
};

class TestApplication : public Application {
public:
    TestApplication(Session *session, const QString &name)
    {
        _pad.reset(new TestPad(session));
        _contentName = name;
        _creator     = session->role();
        _senders     = Origin::Both;
        _state       = State::Pending;
    }
    void                                setState(State state) override { _state = state; }
    const std::optional<Stanza::Error> &lastError() const override { return error; }
    Reason                              lastReason() const override { return {}; }
    SetDescError                        setRemoteOffer(const QDomElement &) override { return Unparsed; }
    SetDescError                        setRemoteAnswer(const QDomElement &) override { return Unparsed; }
    QDomElement                         makeLocalOffer() override { return {}; }
    QDomElement                         makeLocalAnswer() override { return {}; }
    Update                              evaluateOutgoingUpdate() override { return { Action::NoAction, {} }; }
    void                                prepare() override { }
    void                                start() override { }
    void                                remove(Reason::Condition, const QString &) override { }
    void                                incomingRemove(const Reason &reason) override { removedReason = reason; }
    bool                                incomingDescriptionInfo(const QDomElement &description) override
    {
        ++hints;
        lastHint = description;
        return understandsHints;
    }
    bool        understandsHints = true;
    bool        supportsContentModify() const override { return understandsModify; }
    bool        understandsModify = true;
    int         hints             = 0;
    QDomElement lastHint;
    Reason      removedReason;

protected:
    void prepareTransport() override { }

private:
    std::optional<Stanza::Error> error;
};

struct OwnedXml {
    QDomDocument doc;
    QDomElement  root;

    operator QDomElement() const { return root; }
    QDomElement firstChildElement() const { return root.firstChildElement(); }
};

static OwnedXml payload(const QString &contents)
{
    OwnedXml xml;
    check(xml.doc.setContent(QStringLiteral("<jingle xmlns='urn:xmpp:jingle:1'>") + contents
                                 + QStringLiteral("</jingle>"),
                             true),
          "invalid test XML");
    xml.root = xml.doc.documentElement();
    return xml;
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCA::Initializer qca;
    Client           client;
    Session          session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));
    auto             first  = new TestApplication(&session, QStringLiteral("first"));
    auto             second = new TestApplication(&session, QStringLiteral("second"));
    session.addContent(first);
    session.addContent(second);

    const QList<QPair<QString, Origin>> directions { { QStringLiteral("none"), Origin::None },
                                                     { QStringLiteral("both"), Origin::Both },
                                                     { QStringLiteral("initiator"), Origin::Initiator },
                                                     { QStringLiteral("responder"), Origin::Responder } };
    for (const auto &direction : directions) {
        auto xml = payload(
            QStringLiteral("<content creator=\"initiator\" name=\"first\" senders=\"%1\"/>").arg(direction.first));
        ContentBase content(xml.firstChildElement());
        check(content.isValid() && content.senders == direction.second, "wrong parsed senders");
        QDomDocument doc;
        ContentBase  roundtrip(content.toXml(&doc, "content"));
        check(roundtrip.isValid() && roundtrip.senders == direction.second, "senders roundtrip failed");
    }
    const auto defaultDirection = payload(QStringLiteral("<content creator=\"initiator\" name=\"first\"/>"));
    check(ContentBase(defaultDirection.firstChildElement()).senders == Origin::Both, "default senders is not both");
    for (const auto &invalid : { QStringLiteral(""), QStringLiteral("invalid") }) {
        auto xml
            = payload(QStringLiteral("<content creator=\"initiator\" name=\"first\" senders=\"%1\"/>").arg(invalid));
        check(!ContentBase(xml.firstChildElement()).isValid(), "invalid senders accepted");
    }

    int directionNotifications = 0;
    QObject::connect(first, &Application::sendersChanged, first, [&](Origin) { ++directionNotifications; });
    const auto modify
        = payload(QStringLiteral("<content creator=\"initiator\" name=\"first\" senders=\"responder\"/>"));
    const auto beforeModify = session.state();
    check(session.updateFromXml(Action::ContentModify, modify), "content-modify rejected");
    check(first->senders() == Origin::Responder && second->senders() == Origin::Both,
          "direction update routed incorrectly");
    check(first->state() == State::Pending && session.state() == beforeModify,
          "direction update changed negotiation state");
    check(directionNotifications == 1, "missing direction notification");
    check(session.updateFromXml(Action::ContentModify, modify), "repeated direction update rejected");
    check(directionNotifications == 1, "unchanged direction notified again");
    check(session.updateFromXml(Action::ContentModify, defaultDirection), "default direction update rejected");
    check(first->senders() == Origin::Both, "default direction update did not restore both");
    for (const auto &invalidContent :
         { QStringLiteral("<content creator=\"initiator\" name=\"missing\" senders=\"none\"/>"),
           QStringLiteral("<content creator=\"initiator\" name=\"second\" senders=\"invalid\"/>"),
           QStringLiteral("<content creator=\"initiator\" name=\"first\" senders=\"none\"/>"),
           QStringLiteral("<content creator=\"responder\" name=\"second\" senders=\"none\"/>") }) {
        auto batch = payload(QStringLiteral("<content creator=\"initiator\" name=\"first\" senders=\"none\"/>")
                             + invalidContent);
        check(!session.updateFromXml(Action::ContentModify, batch), "invalid direction batch accepted");
        check(first->senders() == Origin::Both, "invalid direction batch partially applied");
    }
    second->understandsModify = false;
    auto unsupportedBatch
        = payload(QStringLiteral("<content creator=\"initiator\" name=\"first\" senders=\"none\"/>"
                                 "<content creator=\"initiator\" name=\"second\" senders=\"none\"/>"));
    check(!session.updateFromXml(Action::ContentModify, unsupportedBatch), "unsupported application modified");
    check(first->senders() == Origin::Both && second->senders() == Origin::Both,
          "unsupported direction batch partially applied");
    check(session.lastError()->condition == Stanza::Error::ErrorCond::FeatureNotImplemented,
          "wrong unsupported direction error");
    second->understandsModify = true;
    first->setState(State::Finishing);
    check(!session.updateFromXml(Action::ContentModify, modify), "finishing content modified");
    first->setState(State::Pending);
    check(!session.updateFromXml(Action::ContentModify, payload({})), "empty direction update accepted");
    check(!session.updateFromXml(
              Action::ContentModify,
              payload(QStringLiteral(
                  "<content xmlns=\"urn:other\" creator=\"initiator\" name=\"first\" senders=\"none\"/>"))),
          "foreign content changed direction");
    check(!session.updateFromXml(
              Action::ContentModify,
              payload(QStringLiteral(
                  "<content creator=\"initiator\" name=\"first\" senders=\"none\"><transport/></content>"))),
          "transport replacement accepted as content-modify");

    const auto groupsXml = payload(QStringLiteral("<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='BUNDLE'>"
                                                  "<content name='video'/><content name='audio'/></group>"
                                                  "<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='BUNDLE'>"
                                                  "<content name='screen'/></group>"
                                                  "<content creator='initiator' name='video'/>"
                                                  "<content creator='initiator' name='audio'/>"
                                                  "<content creator='initiator' name='screen'/>"));
    const auto groups    = Session::parseGroupings(groupsXml);
    check(groups && groups->size() == 2, "same-semantics groups were collapsed");
    check(Session::validBundleAnswer(*groups, *groups), "identical BUNDLE answer rejected");
    check(Session::validBundleAnswer(*groups, {}), "BUNDLE refusal rejected");
    check(Session::validBundleAnswer(*groups, { { "BUNDLE", { "audio" } }, { "BUNDLE", { "screen" } } }),
          "subset answer rejected");
    check(!Session::validBundleAnswer(*groups, { { "BUNDLE", { "audio", "screen" } } }),
          "answer merged offered groups");
    check(!Session::validBundleAnswer(*groups, { { "BUNDLE", { "audio" } }, { "BUNDLE", { "video" } } }),
          "answer split an offered group");
    check(!Session::validBundleAnswer(*groups, { { "BUNDLE", { "unoffered" } } }), "answer added an unoffered member");
    check(!Session::validBundleAnswer({}, *groups), "unsolicited BUNDLE answer accepted");
    check(!Session::validBundleAnswer(*groups, { { "BUNDLE", { "audio", "audio" } } }), "answer repeated a member");
    check(groups->first().contents == QStringList({ "video", "audio" }), "group member order lost");
    check(session.setGroupings(*groups), "valid local groups rejected");
    check(session.groupings().size() == 2, "local groups collapsed");
    check(!session.validLocalGroupings(), "outgoing groups reference nonexistent local contents");
    check(!session.setGroupings({ { "BUNDLE", { "first" } }, { "BUNDLE", { "first", "second" } } }),
          "local content assigned to multiple BUNDLE transports");
    check(session.groupings().size() == 2, "invalid local membership changed proposal");
    check(session.remoteGroupings().isEmpty(), "local proposal became a peer agreement");
    // A configured proposal is not an offer until it is actually sent.
    const auto beforeUnsentAnswer = session.state();
    check(!session.updateFromXml(Action::SessionAccept, groupsXml), "unsent proposal authorized a BUNDLE answer");
    check(session.state() == beforeUnsentAnswer && session.remoteGroupings().isEmpty(),
          "invalid BUNDLE answer changed session state");
    check(!session.setGroupings({ ContentGroup { "BUNDLE", { "audio", "audio" } } }), "duplicate member accepted");
    check(session.groupings().size() == 2, "invalid setter changed local groups");
    session.setGrouping(QStringLiteral("BUNDLE"), { QStringLiteral("first") });
    check(session.groupings().size() == 1, "compatibility setter did not replace matching groups");
    check(session.validLocalGroupings(), "valid local group rejected before send");
    first->setState(State::Finished);
    check(!session.validLocalGroupings(), "outgoing group references a finished content");
    first->setState(State::Pending);
    session.setGrouping(QStringLiteral("BUNDLE"), {});
    check(session.groupings().isEmpty(), "clearing grouping retained an empty group");
    Session responder(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")), Origin::Responder);
    check(!responder.setGroupings({ { "BUNDLE", { "first" } } }), "responder configured an unoffered BUNDLE group");
    check(responder.groupings().isEmpty(), "rejected local answer changed responder proposal");
    check(responder.setGroupings({}), "responder cannot decline grouping");
    const auto malformedGroups
        = payload(QStringLiteral("<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='BUNDLE'>"
                                 "<content name='first'/><content name='first'/></group>"));
    check(!Session::parseGroupings(malformedGroups), "malformed group parsed");
    const auto stateBeforeGroups = session.state();
    check(!session.updateFromXml(Action::SessionAccept, malformedGroups), "malformed group answer accepted");
    check(session.state() == stateBeforeGroups, "malformed group answer changed session state");
    check(first->state() == State::Pending, "malformed group answer changed application state");
    const auto foreignGroups
        = payload(QStringLiteral("<group xmlns='urn:other' semantics='BUNDLE'><content name='first'/></group>"));
    check(Session::parseGroupings(foreignGroups)->isEmpty(), "foreign extension treated as grouping");
    const auto prefixedGroups
        = payload(QStringLiteral("<g:group xmlns:g='urn:xmpp:jingle:apps:grouping:0' semantics='LS'>"
                                 "<g:content name='first'/></g:group><content creator='initiator' name='first'/>"));
    const auto prefixed = Session::parseGroupings(prefixedGroups);
    check(prefixed && !prefixed->isEmpty() && prefixed->first().semantics == QLatin1String("LS"),
          "prefixed grouping not parsed");
    const auto unresolved = Session::parseGroupings(
        payload(QStringLiteral("<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='BUNDLE'>"
                               "<content name='first'/><content name='missing'/></group>"
                               "<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='LS'>"
                               "<content name='first'/></group><content creator='initiator' name='first'/>")));
    check(unresolved && unresolved->size() == 1 && unresolved->first().semantics == QLatin1String("LS"),
          "unresolved group was partially retained or removed a valid sibling group");
    const auto ambiguous = Session::parseGroupings(payload(QStringLiteral(
        "<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='BUNDLE'><content name='same'/></group>"
        "<content creator='initiator' name='same'/><content creator='responder' name='same'/>")));
    check(ambiguous && ambiguous->isEmpty(), "ambiguous group reference resolved by creator implicitly");
    const auto foreignContent = Session::parseGroupings(payload(QStringLiteral(
        "<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='BUNDLE'><content name='first'/></group>"
        "<content xmlns='urn:other' name='first'/>")));
    check(foreignContent && foreignContent->isEmpty(), "foreign content satisfied a group reference");
    const auto emptyGroup = Session::parseGroupings(
        payload(QStringLiteral("<group xmlns='urn:xmpp:jingle:apps:grouping:0' semantics='LS'/>")));
    check(emptyGroup && emptyGroup->size() == 1 && emptyGroup->first().contents.isEmpty(),
          "empty framework group rejected");

    const auto hint = payload(QStringLiteral(
        "<content creator='initiator' name='first'>"
        "<description xmlns='urn:iris:test:application'><parameter name='width' value='640'/></description>"
        "</content>"));
    check(session.updateFromXml(Action::DescriptionInfo, hint), "supported description-info rejected");
    check(first->hints == 1 && second->hints == 0, "description-info routed to wrong content");
    check(first->state() == State::Pending, "description-info changed negotiation state");
    first->understandsHints = false;
    check(!session.updateFromXml(Action::DescriptionInfo, hint), "unsupported hint acknowledged");
    check(session.lastError()->condition == Stanza::Error::ErrorCond::FeatureNotImplemented,
          "wrong unsupported-info error");
    const auto malformedBatch = payload(QStringLiteral(
        "<content creator='initiator' name='first'><description xmlns='urn:iris:test:application'/></content>"
        "<content creator='initiator' name='second'/>"));
    const int  previousHints  = first->hints;
    check(!session.updateFromXml(Action::DescriptionInfo, malformedBatch), "malformed batch accepted");
    check(first->hints == previousHints, "malformed batch partially dispatched");

    first->setState(State::Active);
    const auto reject = payload(QStringLiteral("<content creator='initiator' name='first'/>"));
    check(!session.updateFromXml(Action::ContentReject, reject), "active content rejected as pending offer");
    check(session.contentList().size() == 2, "invalid rejection removed content");
    first->setState(State::Pending);
    first->markInitialApplication(true);
    check(!session.updateFromXml(Action::ContentReject, reject), "initial offer treated as content-add");
    first->markInitialApplication(false);
    QPointer<TestApplication> guard(first);
    check(session.updateFromXml(Action::ContentReject, reject), "pending content rejection failed");
    check(!guard && session.contentList().size() == 1, "rejected content retained");
    check(session.content(QStringLiteral("second"), Origin::Initiator) == second, "unrelated content removed");

    // Notification handlers may close the UI/session or remove another content.
    // Both the generic direction signal and the peer-specific one must tolerate it.
    for (bool peerSpecific : { false, true }) {
        for (bool destroySession : { false, true }) {
            auto disposable = new Session(client.jingleManager(), Jid(QStringLiteral("peer@example.org/device")));
            QPointer<Session> sessionGuard(disposable);
            auto              a = new TestApplication(disposable, QStringLiteral("first"));
            auto              b = new TestApplication(disposable, QStringLiteral("second"));
            disposable->addContent(a);
            disposable->addContent(b);
            QPointer<TestApplication> bGuard(b);
            auto                      destroy = [=](Origin) {
                if (destroySession)
                    delete disposable;
                else
                    delete b;
            };
            if (peerSpecific)
                QObject::connect(a, &Application::sendersChangedByPeer, a, destroy);
            else
                QObject::connect(a, &Application::sendersChanged, a, destroy);
            check(disposable->updateFromXml(Action::ContentModify, unsupportedBatch),
                  "content removal during direction notification failed");
            check(!bGuard, "notification did not remove content");
            if (sessionGuard)
                delete disposable;
        }
    }

    const Hash              hashA(Hash::Sha256, QByteArray(32, 'a'));
    const Hash              hashB(Hash::Sha256, QByteArray(32, 'b'));
    const Dtls::FingerPrint a(hashA, Dtls::Active);
    check(a == Dtls::FingerPrint(hashA, Dtls::Active), "identical fingerprints differ");
    check(a != Dtls::FingerPrint(hashB, Dtls::Active), "changed certificate ignored");
    check(a != Dtls::FingerPrint(hashA, Dtls::Passive), "changed DTLS role ignored");
    qInfo("Jingle signaling regressions passed");
}
