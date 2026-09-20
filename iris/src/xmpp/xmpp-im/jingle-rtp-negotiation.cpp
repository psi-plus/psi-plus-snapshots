// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-negotiation.h"

#include <QSet>
#include <QVector>

namespace XMPP::Jingle::RTP {
namespace {
    bool sameParameters(const QList<ExtensionParameter> &a, const QList<ExtensionParameter> &b)
    {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i) {
            if (a.at(i).name != b.at(i).name || a.at(i).value != b.at(i).value)
                return false;
        }
        return true;
    }

    bool sameFeedback(const Feedback &a, const Feedback &b)
    {
        return a.type == b.type && a.subtype == b.subtype && sameParameters(a.parameters, b.parameters);
    }

    bool feedbackSubset(const QList<Feedback> &offered, const QList<Feedback> &answered)
    {
        QVector<bool> used(offered.size(), false);
        for (const auto &answer : answered) {
            bool matched = false;
            for (int i = 0; i < offered.size(); ++i) {
                if (!used.at(i) && sameFeedback(offered.at(i), answer)) {
                    used[i] = true;
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        return true;
    }

    bool hasSpecificFeedback(const Description &description)
    {
        if (!description.feedback.isEmpty())
            return true;
        for (const auto &payload : description.payloads)
            if (!payload.feedback.isEmpty())
                return true;
        return false;
    }

    bool hasAnyFeedbackTrrInt(const Description &description)
    {
        if (description.feedbackTrrInt)
            return true;
        for (const auto &payload : description.payloads)
            if (payload.feedbackTrrInt)
                return true;
        return false;
    }

    bool feedbackCompatible(const Description &offer, const Description &answer)
    {
        if (!feedbackSubset(offer.feedback, answer.feedback))
            return false;

        const bool syntheticDefaultTrr = answer.feedbackTrrInt && !offer.feedbackTrrInt;
        if (answer.feedbackTrrInt && offer.feedbackTrrInt && answer.feedbackTrrInt != offer.feedbackTrrInt)
            return false;

        for (const auto &selected : answer.payloads) {
            const PayloadType *offered = nullptr;
            for (const auto &candidate : offer.payloads) {
                if (candidate.id == selected.id) {
                    offered = &candidate;
                    break;
                }
            }
            if (!offered)
                return false;
            if (!feedbackSubset(offered->feedback, selected.feedback))
                return false;
            if (selected.feedbackTrrInt
                && (!offered->feedbackTrrInt || selected.feedbackTrrInt != offered->feedbackTrrInt))
                return false;
        }

        if (!syntheticDefaultTrr)
            return true;

        // XEP-0293 allows a responder that removes every specific feedback
        // request to retain RTP/AVPF by returning a content-level trr-int=0,
        // but only when the offer had no trr-int value of its own.
        if (*answer.feedbackTrrInt != 0 || hasAnyFeedbackTrrInt(offer) || !hasSpecificFeedback(offer)
            || hasSpecificFeedback(answer))
            return false;
        for (const auto &payload : answer.payloads)
            if (payload.feedbackTrrInt)
                return false;
        return true;
    }

    bool sameHeaderConfiguration(const HeaderExtension &a, const HeaderExtension &b)
    {
        return a.uri == b.uri && sameParameters(a.parameters, b.parameters);
    }

    bool compatibleSenders(Origin offered, Origin answered)
    {
        if (offered == Origin::Both)
            return answered == Origin::Both || answered == Origin::Initiator || answered == Origin::Responder;
        return offered == answered;
    }

    bool isUsableHeaderId(quint16 id) { return id >= 1 && id <= 256; }
    bool isExtendedHeaderId(quint16 id) { return id >= 4096 && id <= 4351; }

    bool validHeaderMap(const Description &description)
    {
        QSet<quint16> usableIds;
        for (const auto &extension : description.headerExtensions) {
            if (isUsableHeaderId(extension.id)) {
                if (usableIds.contains(extension.id))
                    return false;
                usableIds.insert(extension.id);
            } else if (!isExtendedHeaderId(extension.id)) {
                return false;
            }
        }
        return true;
    }

    bool headerExtensionsCompatible(const Description &offer, const Description &answer)
    {
        if (answer.extmapAllowMixed && !offer.extmapAllowMixed)
            return false;

        QVector<bool> used(offer.headerExtensions.size(), false);
        QSet<quint16> selectedExtendedIds;
        for (const auto &selected : answer.headerExtensions) {
            int matched = -1;
            for (int i = 0; i < offer.headerExtensions.size(); ++i) {
                const auto &candidate = offer.headerExtensions.at(i);
                if (used.at(i) || !sameHeaderConfiguration(candidate, selected)
                    || !compatibleSenders(candidate.senders, selected.senders))
                    continue;

                if (isUsableHeaderId(candidate.id)) {
                    if (selected.id != candidate.id)
                        continue;
                } else {
                    if (!isExtendedHeaderId(candidate.id))
                        continue;
                    // Extended identifiers are offer-only alternatives. They can
                    // either be echoed as capability information or remapped to a
                    // free usable id by the answerer (RFC 8285 section 7).
                    if (selected.id != candidate.id && !isUsableHeaderId(selected.id))
                        continue;
                    if (selectedExtendedIds.contains(candidate.id))
                        continue;
                }
                matched = i;
                break;
            }
            if (matched < 0)
                return false;
            used[matched]        = true;
            const auto offeredId = offer.headerExtensions.at(matched).id;
            if (isExtendedHeaderId(offeredId))
                selectedExtendedIds.insert(offeredId);
        }
        return true;
    }

    // Validate programmatically constructed descriptions as well as parsed XML.
    // The XML roundtrip also detaches the implicitly shared extension DOM nodes.
    std::optional<Description> snapshot(const Description &description)
    {
        if (description.rtcpMux) {
            for (const auto &payload : description.payloads)
                if (payload.id >= 64 && payload.id <= 95)
                    return {}; // RFC 5761 section 4: RTP/RTCP demultiplexing conflict.
        }
        if (!validHeaderMap(description))
            return {};
        QDomDocument doc;
        return Description::fromXml(description.toXml(doc));
    }

    bool compatible(const Description &offer, const Description &answer)
    {
        if (offer.media != answer.media || (answer.rtcpMux && !offer.rtcpMux) || !feedbackCompatible(offer, answer)
            || !headerExtensionsCompatible(offer, answer))
            return false;
        for (const auto &selected : answer.payloads) {
            const PayloadType *offered = nullptr;
            for (const auto &candidate : offer.payloads) {
                if (candidate.id == selected.id) {
                    offered = &candidate;
                    break;
                }
            }
            if (!offered)
                return false;
            // Static payloads may omit codec metadata. The media adapter resolves
            // those assignments. Dynamic payloads must identify the same codec.
            if (!offered->name.isEmpty() && !selected.name.isEmpty()
                && offered->name.compare(selected.name, Qt::CaseInsensitive) != 0)
                return false;
            if (offered->clockrate && selected.clockrate && offered->clockrate != selected.clockrate)
                return false;
            if (offered->channels.value_or(1) != selected.channels.value_or(1))
                return false;
        }
        return true;
    }
}

Negotiation::Result Negotiation::setLocalOffer(const Description &offer)
{
    if (state_ != State::Empty)
        return Result::WrongState;
    auto value = snapshot(offer);
    if (!value)
        return Result::InvalidDescription;
    local_ = std::move(value);
    state_ = State::Offered;
    return Result::Ok;
}

Negotiation::Result Negotiation::setRemoteOffer(const Description &offer, const CodecNegotiator &codecs)
{
    if (state_ != State::Empty)
        return Result::WrongState;
    auto value = snapshot(offer);
    if (!value)
        return Result::InvalidDescription;
    // Give the adapter its own DOM copy, not the snapshot being committed.
    auto adapterOffer = snapshot(*value);
    if (!adapterOffer)
        return Result::InvalidDescription;
    auto proposed = codecs.makeAnswer(*adapterOffer);
    if (!proposed)
        return Result::UnsupportedMedia;
    return setRemoteOffer(*value, *proposed);
}

Negotiation::Result Negotiation::setRemoteOffer(const Description &offer, const Description &localAnswer)
{
    if (state_ != State::Empty)
        return Result::WrongState;
    auto remote = snapshot(offer);
    if (!remote)
        return Result::InvalidDescription;
    auto local = snapshot(localAnswer);
    if (!local)
        return Result::InvalidDescription;
    if (!compatible(*remote, *local))
        return Result::IncompatibleAnswer;
    remote_ = std::move(remote);
    local_  = std::move(local);
    state_  = State::Accepted;
    return Result::Ok;
}

Negotiation::Result Negotiation::setRemoteAnswer(const Description &description, const CodecNegotiator &codecs)
{
    if (state_ != State::Offered)
        return Result::WrongState;
    auto answer = snapshot(description);
    if (!answer)
        return Result::InvalidDescription;
    if (!compatible(*local_, *answer))
        return Result::IncompatibleAnswer;
    auto adapterOffer  = snapshot(*local_);
    auto adapterAnswer = snapshot(*answer);
    if (!adapterOffer || !adapterAnswer)
        return Result::InvalidDescription;
    if (!codecs.acceptsAnswer(*adapterOffer, *adapterAnswer))
        return Result::UnsupportedMedia;
    remote_ = std::move(answer);
    state_  = State::Accepted;
    return Result::Ok;
}

std::optional<Description> Negotiation::localDescription() const { return local_ ? snapshot(*local_) : std::nullopt; }

std::optional<Description> Negotiation::remoteDescription() const
{
    return remote_ ? snapshot(*remote_) : std::nullopt;
}
}
