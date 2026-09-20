// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <iris/jingle-rtp-negotiation.h>

using XMPP::Jingle::Origin;
using namespace XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}
using Result = Negotiation::Result;

class MockCodecs : public CodecNegotiator {
public:
    mutable int                validations = 0;
    bool                       accept = true;
    std::optional<Description> response;
    std::optional<Description> makeAnswer(const Description &) const override { return response; }
    bool acceptsAnswer(const Description &, const Description &) const override
    {
        ++validations;
        return accept;
    }
};

static Description makeOffer()
{
    Description result;
    result.media   = "audio";
    result.rtcpMux = true;
    PayloadType opus;
    opus.id        = 111;
    opus.name      = "opus";
    opus.clockrate = 48000;
    opus.channels  = 2;
    opus.parameters.insert("useinbandfec", "1");
    PayloadType pcmu;
    pcmu.id         = 0;
    result.payloads = { opus, pcmu };
    result.extensions.append(QByteArrayLiteral("<test xmlns=\"urn:iris:test\" value=\"original\"/>"));
    return result;
}

static Result answerResult(const Description &offer, const Description &answer)
{
    MockCodecs  codecs;
    Negotiation negotiation;
    const auto  initial = negotiation.setLocalOffer(offer);
    if (initial != Result::Ok)
        return initial;
    return negotiation.setRemoteAnswer(answer, codecs);
}

static Feedback feedback(const char *type, const char *subtype = nullptr)
{
    Feedback result;
    result.type = QString::fromLatin1(type);
    if (subtype)
        result.subtype = QString::fromLatin1(subtype);
    return result;
}

static HeaderExtension headerExtension(quint16 id, const char *uri, Origin senders = Origin::Both)
{
    HeaderExtension result;
    result.id      = id;
    result.uri     = QString::fromLatin1(uri);
    result.senders = senders;
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto             offer = makeOffer();
    for (int pt : { 64, 72, 95 }) {
        auto conflict                = makeOffer();
        conflict.payloads.first().id = pt;
        Negotiation muxed;
        check(muxed.setLocalOffer(conflict) == Result::InvalidDescription, "ambiguous mux payload offered");
        conflict.rtcpMux = false;
        Negotiation separate;
        check(separate.setLocalOffer(conflict) == Result::Ok, "mux restriction leaked into separate components");
    }
    auto answer = makeOffer();
    answer.payloads.removeLast();
    answer.payloads.first().parameters.insert("minptime", "10");
    answer.payloads.first().name = "OPUS";
    answer.ssrc                  = 42; // Peer SSRC is independent.
    MockCodecs  codecs;
    Negotiation initiator;
    check(initiator.setRemoteAnswer(answer, codecs) == Result::WrongState, "unsolicited answer accepted");
    check(initiator.setLocalOffer(offer) == Result::Ok, "local offer rejected");
    check(initiator.setLocalOffer(offer) == Result::WrongState, "outstanding offer replaced");
    offer.extensions.first() = QByteArrayLiteral("<test xmlns=\"urn:iris:test\" value=\"changed-by-caller\"/>");
    check(initiator.localDescription()->extensions.first().contains("value=\"original\""),
          "caller changed stored offer");
    auto copy = initiator.localDescription();
    copy->extensions.first() = QByteArrayLiteral("<test xmlns=\"urn:iris:test\" value=\"changed-by-getter\"/>");
    check(initiator.localDescription()->extensions.first().contains("value=\"original\""),
          "getter leaked mutable opaque XML");

    for (int kind = 0; kind < 7; ++kind) {
        auto invalid = answer;
        if (kind == 0)
            invalid.media = "video";
        if (kind == 1)
            invalid.payloads.first().id = 112;
        if (kind == 2)
            invalid.payloads.first().name = "VP8";
        if (kind == 3)
            invalid.payloads.first().clockrate = 16000;
        if (kind == 4)
            invalid.payloads.first().channels = 1;
        if (kind == 5)
            invalid.payloads.clear();
        if (kind == 6) {
            const auto duplicate = invalid.payloads.first();
            invalid.payloads.append(duplicate);
        }
        check(initiator.setRemoteAnswer(invalid, codecs) != Result::Ok, "invalid answer accepted");
        check(initiator.state() == Negotiation::State::Offered && !initiator.remoteDescription(),
              "invalid answer changed committed state");
        check(codecs.validations == 0, "malformed answer reached media adapter");
    }
    codecs.accept = false;
    check(initiator.setRemoteAnswer(answer, codecs) == Result::UnsupportedMedia, "media veto ignored");
    check(initiator.state() == Negotiation::State::Offered, "media veto consumed offer");
    codecs.accept = true;
    check(initiator.setRemoteAnswer(answer, codecs) == Result::Ok, "valid codec-specific answer rejected");
    check(initiator.localDescription()->extensions.first().contains("value=\"original\""),
          "stored offer extension changed during adapter validation");
    check(initiator.remoteDescription()->payloads.first().parameters.value("minptime") == "10",
          "codec-specific answer parameters discarded");
    check(initiator.setRemoteAnswer(answer, codecs) == Result::WrongState, "second answer accepted");
    Negotiation responder;
    check(responder.setRemoteOffer(makeOffer(), codecs) == Result::UnsupportedMedia, "missing codec accepted");
    check(responder.state() == Negotiation::State::Empty, "failed offer committed");
    codecs.response                      = answer;
    codecs.response->payloads.first().id = 112;
    check(responder.setRemoteOffer(makeOffer(), codecs) == Result::IncompatibleAnswer,
          "adapter invented an unoffered payload");
    check(!responder.localDescription() && !responder.remoteDescription(), "invalid local answer committed");
    codecs.response = answer;
    check(responder.setRemoteOffer(makeOffer(), codecs) == Result::Ok, "responder negotiation failed");
    check(responder.remoteDescription()->extensions.first().contains("value=\"original\""),
          "answer factory changed stored remote offer");
    check(responder.localDescription()->ssrc == 42, "local answer not stored separately");
    auto noMux    = makeOffer();
    noMux.rtcpMux = false;
    Negotiation unsolicitedMux;
    check(unsolicitedMux.setLocalOffer(noMux) == Result::Ok, "unmuxed offer rejected");
    check(unsolicitedMux.setRemoteAnswer(answer, codecs) == Result::IncompatibleAnswer, "unsolicited mux accepted");
    Negotiation declinedMux;
    check(declinedMux.setLocalOffer(makeOffer()) == Result::Ok, "mux offer rejected");
    answer.rtcpMux = false;
    check(declinedMux.setRemoteAnswer(answer, codecs) == Result::Ok, "mux refusal rejected");
    Negotiation reordered;
    check(reordered.setLocalOffer(makeOffer()) == Result::Ok, "offer rejected");
    auto reorderedAnswer = makeOffer();
    reorderedAnswer.payloads.swapItemsAt(0, 1);
    check(reordered.setRemoteAnswer(reorderedAnswer, codecs) == Result::Ok, "peer codec preferences rejected");
    check(reordered.remoteDescription()->payloads.first().id == 0, "peer codec preference order lost");
    // XEP-0293: accepted feedback is an unchanged subset at the same scope.
    auto feedbackOffer = makeOffer();
    feedbackOffer.feedback.append(feedback("nack", "pli"));
    feedbackOffer.feedback.first().parameters.append({ QStringLiteral("mode"), QStringLiteral("fast") });
    feedbackOffer.feedbackTrrInt = 100;
    feedbackOffer.payloads.first().feedback.append(feedback("transport-cc"));
    feedbackOffer.payloads.first().feedbackTrrInt = 50;
    auto feedbackAnswer                           = feedbackOffer;
    feedbackAnswer.feedback.clear();
    feedbackAnswer.payloads.first().feedback.clear();
    check(answerResult(feedbackOffer, feedbackAnswer) == Result::Ok, "feedback subset rejected");

    auto modifiedFeedback                     = feedbackOffer;
    modifiedFeedback.feedback.first().subtype = QStringLiteral("sli");
    check(answerResult(feedbackOffer, modifiedFeedback) == Result::IncompatibleAnswer,
          "modified description feedback accepted");
    modifiedFeedback                                           = feedbackOffer;
    modifiedFeedback.feedback.first().parameters.first().value = QStringLiteral("slow");
    check(answerResult(feedbackOffer, modifiedFeedback) == Result::IncompatibleAnswer,
          "modified feedback parameter accepted");
    modifiedFeedback                = feedbackOffer;
    modifiedFeedback.feedbackTrrInt = 101;
    check(answerResult(feedbackOffer, modifiedFeedback) == Result::IncompatibleAnswer,
          "modified description trr-int accepted");
    modifiedFeedback                                 = feedbackOffer;
    modifiedFeedback.payloads.first().feedbackTrrInt = 51;
    check(answerResult(feedbackOffer, modifiedFeedback) == Result::IncompatibleAnswer,
          "modified payload trr-int accepted");
    modifiedFeedback = feedbackOffer;
    modifiedFeedback.payloads.first().feedback.append(feedback("goog-remb"));
    check(answerResult(feedbackOffer, modifiedFeedback) == Result::IncompatibleAnswer,
          "unoffered payload feedback accepted");

    auto avpfOffer = makeOffer();
    avpfOffer.feedback.append(feedback("nack", "pli"));
    auto avpfOnly           = makeOffer();
    avpfOnly.feedbackTrrInt = 0;
    check(answerResult(avpfOffer, avpfOnly) == Result::Ok, "XEP-0293 AVPF trr-int=0 fallback rejected");
    check(answerResult(makeOffer(), avpfOnly) == Result::IncompatibleAnswer, "unsolicited AVPF trr-int=0 accepted");
    auto offeredTrr           = avpfOffer;
    offeredTrr.feedbackTrrInt = 100;
    check(answerResult(offeredTrr, avpfOnly) == Result::IncompatibleAnswer,
          "offered trr-int was replaced by synthetic zero");
    // XEP-0294 / RFC 8285: ordinary ids remain stable while extended offer ids
    // may represent alternatives and are remapped to a free usable answer id.
    auto headerOffer = makeOffer();
    auto mid         = headerExtension(1, "urn:ietf:params:rtp-hdrext:sdes:mid");
    mid.parameters.append({ QStringLiteral("mode"), QStringLiteral("compact") });
    headerOffer.headerExtensions.append(mid);
    headerOffer.extmapAllowMixed                  = true;
    auto headerAnswer                             = headerOffer;
    headerAnswer.headerExtensions.first().senders = Origin::Initiator;
    headerAnswer.extmapAllowMixed                 = false;
    check(answerResult(headerOffer, headerAnswer) == Result::Ok, "header-extension sender downgrade rejected");

    auto badHeader                        = headerAnswer;
    badHeader.headerExtensions.first().id = 2;
    check(answerResult(headerOffer, badHeader) == Result::IncompatibleAnswer,
          "ordinary header-extension id remap accepted");
    badHeader                              = headerAnswer;
    badHeader.headerExtensions.first().uri = QStringLiteral("urn:example:unoffered");
    check(answerResult(headerOffer, badHeader) == Result::IncompatibleAnswer,
          "unoffered header-extension URI accepted");
    badHeader                                                   = headerAnswer;
    badHeader.headerExtensions.first().parameters.first().value = QStringLiteral("changed");
    check(answerResult(headerOffer, badHeader) == Result::IncompatibleAnswer,
          "modified header-extension parameter accepted");
    badHeader                                  = headerAnswer;
    badHeader.headerExtensions.first().senders = Origin::None;
    check(answerResult(headerOffer, badHeader) == Result::IncompatibleAnswer,
          "unsupported both-to-none sender downgrade accepted");
    auto noMixedOffer             = headerOffer;
    noMixedOffer.extmapAllowMixed = false;
    auto addedMixed               = headerAnswer;
    addedMixed.extmapAllowMixed   = true;
    check(answerResult(noMixedOffer, addedMixed) == Result::IncompatibleAnswer,
          "unoffered extmap-allow-mixed accepted");

    auto directionalOffer = makeOffer();
    directionalOffer.headerExtensions.append(
        headerExtension(3, "urn:ietf:params:rtp-hdrext:ssrc-audio-level", Origin::Initiator));
    auto directionalAnswer                             = directionalOffer;
    directionalAnswer.headerExtensions.first().senders = Origin::Responder;
    check(answerResult(directionalOffer, directionalAnswer) == Result::IncompatibleAnswer,
          "one-way header-extension direction was reversed");

    auto extendedOffer = makeOffer();
    extendedOffer.headerExtensions
        = { headerExtension(4096, "urn:example:gps-string"), headerExtension(4096, "urn:example:gps-binary") };
    auto extendedAnswer             = makeOffer();
    extendedAnswer.headerExtensions = { headerExtension(2, "urn:example:gps-string", Origin::Responder) };
    check(answerResult(extendedOffer, extendedAnswer) == Result::Ok, "extended header alternative remap rejected");
    auto tooManyAlternatives = extendedAnswer;
    tooManyAlternatives.headerExtensions.append(headerExtension(3, "urn:example:gps-binary"));
    check(answerResult(extendedOffer, tooManyAlternatives) == Result::IncompatibleAnswer,
          "multiple alternatives from one extended id accepted");
    auto echoedExtended             = makeOffer();
    echoedExtended.headerExtensions = { headerExtension(4096, "urn:example:gps-binary") };
    check(answerResult(extendedOffer, echoedExtended) == Result::Ok, "extended capability echo rejected");

    auto duplicateUsableIds             = makeOffer();
    duplicateUsableIds.headerExtensions = { headerExtension(1, "urn:example:a"), headerExtension(1, "urn:example:b") };
    Negotiation invalidHeaderOffer;
    check(invalidHeaderOffer.setLocalOffer(duplicateUsableIds) == Result::InvalidDescription,
          "duplicate usable header-extension ids accepted");
    Negotiation extendedAlternatives;
    check(extendedAlternatives.setLocalOffer(extendedOffer) == Result::Ok,
          "duplicate extended alternative ids rejected");
    // SSRC/source information is endpoint state, not an offer capability: peers
    // are free to describe different local sources in the answer.
    auto sourceOffer = makeOffer();
    sourceOffer.sources.append(Source { 111, {} });
    auto sourceAnswer    = sourceOffer;
    sourceAnswer.sources = { Source { 222, {} } };
    check(answerResult(sourceOffer, sourceAnswer) == Result::Ok, "independent peer SSRC source rejected");
    Negotiation invalidOffer;
    auto        empty = makeOffer();
    empty.payloads.clear();
    check(invalidOffer.setLocalOffer(empty) == Result::InvalidDescription, "empty offer accepted");
    check(invalidOffer.state() == Negotiation::State::Empty, "invalid offer changed state");
    qInfo("RTP negotiation regressions passed");
}
