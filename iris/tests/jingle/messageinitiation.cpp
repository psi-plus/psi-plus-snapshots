// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iris/xmpp-im/jingle-rtp.h>
#include <iris/xmpp-im/jingle-session.h>
#include <iris/xmpp-im/jingle.h>
#include <iris/xmpp-im/xmpp_client.h>
#include <iris/xmpp-im/xmpp_jinglemessage.h>
#include <iris/xmpp-im/xmpp_message.h>

#include <QCoreApplication>
#include <QDomDocument>

#include <any>
#include <optional>

using namespace XMPP;
namespace J = XMPP::Jingle;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

static QDomElement parseRoot(const QString &xml, QDomDocument *document)
{
    check(document->setContent(xml, true), "could not parse JMI fixture");
    return document->documentElement();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    J::RTP::Manager rtp;

    {
        Stanza nullStanza;
        check(nullStanza.isNull(), "default Stanza must be null");
        check(nullStanza.element().isNull(), "element() on a null Stanza must be safe");
        Message message;
        check(!message.fromStanza(nullStanza), "Message accepted a null Stanza");
    }

    {
        QDomDocument source;
        const auto root = parseRoot(
            QStringLiteral(
                "<propose xmlns='urn:xmpp:jingle-message:0' id='call-1'>"
                "<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"
                "<future xmlns='urn:iris:test' value='application-owned'/>"
                "</description>"
                "<proposal xmlns='urn:example:jingle:application'>"
                "<nested value='opaque-to-jmi'/>"
                "</proposal>"
                "</propose>"),
            &source);

        auto initiation = J::MessageInitiation::fromXml(
            root, [&rtp](const QDomElement &description) -> std::optional<std::any> {
                if (description.namespaceURI() == J::RTP::Description::ns())
                    return rtp.parseProposal(description);
                return std::nullopt;
            });

        check(initiation.isValid(), "valid JMI propose was rejected");
        check(initiation.action() == J::MessageInitiation::Action::Propose, "propose action was not parsed");
        check(initiation.id() == QStringLiteral("call-1"), "propose id was not parsed");
        check(initiation.descriptions().size() == 2, "proposal descriptions were not retained");

        const auto rtpDescription = initiation.descriptions().at(0);
        check(rtpDescription.applicationNamespace == J::RTP::Description::ns(),
              "RTP proposal namespace was not retained");
        check(rtpDescription.isSupported(), "registered RTP proposal was not parsed");
        const auto rtpProposal = std::any_cast<J::RTP::Proposal>(rtpDescription.data);
        check(rtpProposal.media == J::RTP::Media::Audio, "RTP proposal media was not parsed by RTP manager");

        const auto unknown = initiation.descriptions().at(1);
        check(unknown.applicationNamespace == QStringLiteral("urn:example:jingle:application"),
              "unknown proposal namespace was not retained");
        check(!unknown.isSupported(), "unknown proposal unexpectedly acquired a typed payload");

        source = QDomDocument();
        check(std::any_cast<J::RTP::Proposal>(initiation.descriptions().at(0).data).media
                  == J::RTP::Media::Audio,
              "typed RTP proposal depended on source DOM lifetime");
    }

    {
        const J::RTP::MediaSet media = J::RTP::Media::Audio | J::RTP::Media::Video;
        check(media.testFlag(J::RTP::Media::Audio) && media.testFlag(J::RTP::Media::Video),
              "audio+video RTP media flags did not compose");

        J::MessageInitiation initiation(J::MessageInitiation::Action::Propose, QStringLiteral("call-out"));
        if (media.testFlag(J::RTP::Media::Audio))
            initiation.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Audio });
        if (media.testFlag(J::RTP::Media::Video))
            initiation.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Video });

        QDomDocument target;
        const auto serialized = initiation.toXml(
            &target, [&rtp](const QString &ns, const std::any &data, QDomDocument *document) {
                return ns == J::RTP::Description::ns() ? rtp.serializeProposal(data, document) : QDomElement();
            });
        check(!serialized.isNull(), "typed audio+video RTP proposal could not be serialized");

        const auto audio = serialized.firstChildElement();
        const auto video = audio.nextSiblingElement();
        check(audio.namespaceURI() == J::RTP::Description::ns()
                  && audio.attribute(QStringLiteral("media")) == QStringLiteral("audio"),
              "audio proposal description was not serialized first");
        check(video.namespaceURI() == J::RTP::Description::ns()
                  && video.attribute(QStringLiteral("media")) == QStringLiteral("video"),
              "video proposal description was not serialized second");
        check(video.nextSiblingElement().isNull(), "audio+video proposal emitted extra descriptions");
    }

    {
        QDomDocument document;
        auto unsupported = J::MessageInitiation::fromXml(
            parseRoot(
                QStringLiteral(
                    "<propose xmlns='urn:xmpp:jingle-message:0' id='unknown'>"
                    "<payload xmlns='urn:example:unknown'><nested/></payload>"
                    "</propose>"),
                &document));
        check(unsupported.isValid(), "unknown application proposal should remain structurally valid");
        check(unsupported.descriptions().size() == 1 && !unsupported.descriptions().at(0).isSupported(),
              "unknown application proposal was not marked unsupported");

        QDomDocument output;
        check(unsupported.toXml(&output).isNull(),
              "unsupported proposal must not be re-emitted by inventing application XML");
    }

    {
        QDomDocument document;
        auto reject = J::MessageInitiation::fromXml(parseRoot(
            QStringLiteral(
                "<reject xmlns='urn:xmpp:jingle-message:0' id='call-2'>"
                "<reason xmlns='urn:xmpp:jingle:1'><busy/><text>Already in a call</text></reason>"
                "<tie-break xmlns='urn:xmpp:jingle-message:0'/>"
                "<migrated xmlns='urn:xmpp:jingle-message:0' to='call-3'/>"
                "</reject>"),
            &document));
        check(reject.isValid(), "valid JMI reject was rejected");
        check(reject.action() == J::MessageInitiation::Action::Reject, "reject action was not parsed");
        check(reject.reasonCondition() == QStringLiteral("busy"), "reject reason was not parsed");
        check(reject.reasonText() == QStringLiteral("Already in a call"), "reject reason text was not parsed");
        check(reject.tieBreak(), "tie-break marker was not parsed");
        check(reject.migratedTo() == QStringLiteral("call-3"), "migrated target was not parsed");

        QDomDocument output;
        check(!reject.toXml(&output).isNull(), "non-proposal JMI unexpectedly required an application serializer");
    }

    {
        QDomDocument document;
        auto invalid = J::MessageInitiation::fromXml(
            parseRoot(QStringLiteral("<propose xmlns='urn:xmpp:jingle-message:0' id='empty'/>"), &document));
        check(!invalid.isValid(), "description-less propose was accepted");
    }

    {
        Message message;
        J::MessageInitiation ringing(J::MessageInitiation::Action::Ringing, QStringLiteral("call-4"));
        message.setJingleMessageInitiation(ringing);
        check(message.jingleMessageInitiation().isValid(), "Message did not retain typed JMI payload");
        check(message.jingleMessageInitiation().action() == J::MessageInitiation::Action::Ringing,
              "Message changed typed JMI action");
    }

    {
        Client client;
        auto manager = client.jingleManager();
        auto rtpManager = manager->rtpManager();

        int                  rtpProposalSignals = 0;
        QString              lastProposalId;
        J::RTP::MediaSet     lastProposalMedia;
        QObject::connect(rtpManager, &J::RTP::Manager::incomingProposal, &client,
                         [&](const Message &, const QString &id, J::RTP::MediaSet media) {
                             ++rtpProposalSignals;
                             lastProposalId    = id;
                             lastProposalMedia = media;
                         });

        Message sourceMessage(Jid(QStringLiteral("local@example.test")));
        sourceMessage.setFrom(Jid(QStringLiteral("peer@example.test/device")));

        J::MessageInitiation pureRtp(J::MessageInitiation::Action::Propose, QStringLiteral("pure-rtp"));
        pureRtp.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Audio });
        pureRtp.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Video });
        manager->incomingMessageInitiation(sourceMessage, pureRtp);
        check(rtpProposalSignals == 1 && lastProposalId == QStringLiteral("pure-rtp"),
              "pure RTP proposal did not produce typed RTP signal");
        check(lastProposalMedia.testFlag(J::RTP::Media::Audio)
                  && lastProposalMedia.testFlag(J::RTP::Media::Video),
              "typed RTP signal lost proposed media");

        J::MessageInitiation mixed(J::MessageInitiation::Action::Propose, QStringLiteral("mixed"));
        mixed.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Audio });
        mixed.addDescription(QStringLiteral("urn:example:other"), std::any());
        manager->incomingMessageInitiation(sourceMessage, mixed);
        check(rtpProposalSignals == 1, "mixed application proposal leaked into RTP convenience signal");

        J::MessageInitiation duplicate(J::MessageInitiation::Action::Propose, QStringLiteral("duplicate"));
        duplicate.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Audio });
        duplicate.addDescription(J::RTP::Description::ns(), J::RTP::Proposal { J::RTP::Media::Audio });
        manager->incomingMessageInitiation(sourceMessage, duplicate);
        check(rtpProposalSignals == 1, "duplicate RTP media proposal produced a typed RTP signal");

        check(manager && !manager->messageInitiationEnabled(), "JMI must be opt-in");
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "JMI must not be advertised through disco");
        check(manager->rtpManager()
                  ->propose(Jid(QStringLiteral("peer@example.test")),
                            J::RTP::Media::Audio | J::RTP::Media::Video)
                  .isEmpty(),
              "disabled JMI allowed a new RTP proposal");
        manager->setMessageInitiationEnabled(true);
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "enabled JMI leaked into disco despite XEP-0353 having no discovery feature");
        check(manager->rtpManager()
                  ->propose(Jid(QStringLiteral("peer@example.test")),
                            J::RTP::Media::Audio | J::RTP::Media::Video)
                  .isEmpty(),
              "RTP proposal ignored unavailable local media/transport capability");
        manager->setMessageInitiationEnabled(false);
        check(!manager->discoFeatures().contains(J::MessageInitiation::ns()),
              "JMI unexpectedly appeared in disco");

        const Jid sessionPeer(QStringLiteral("peer@example.test/device"));
        auto fixed = manager->newSession(sessionPeer, QStringLiteral("jmi-session-id"));
        check(fixed && fixed->sid() == QStringLiteral("jmi-session-id"),
              "JMI id was not accepted as an outgoing Jingle sid");
        check(manager->session(sessionPeer, QStringLiteral("jmi-session-id")) == fixed,
              "preselected JMI sid was not registered");
        check(!manager->newSession(sessionPeer, QStringLiteral("jmi-session-id")),
              "duplicate JMI sid was accepted for the same peer");
        delete fixed;
        check(!manager->session(sessionPeer, QStringLiteral("jmi-session-id")),
              "destroyed preselected session remained registered");

        J::MessageInitiation unsupported(J::MessageInitiation::Action::Propose, QStringLiteral("call-5"));
        unsupported.addDescription(QStringLiteral("urn:example:unknown"));
        check(!manager->sendMessageInitiation(Jid(QStringLiteral("peer@example.test")), unsupported),
              "manager sent a proposal that no application can serialize");
    }

    qInfo("XEP-0353 Jingle Message Initiation regressions passed");
    return 0;
}
