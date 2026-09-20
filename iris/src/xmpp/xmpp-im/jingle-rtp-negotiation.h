// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_NEGOTIATION_H
#define JINGLE_RTP_NEGOTIATION_H

#include "jingle-rtp-description.h"

namespace XMPP::Jingle::RTP {

// Implemented by the media adapter, once per content. Calls are synchronous on
// the Jingle thread, without nested event loops, capture or network side effects.
// Codec-specific fmtp, static payload mappings, RTX dependencies and extensions
// must be checked here; an unknown extension must not be blindly accepted.
class IRIS_EXPORT CodecNegotiator {
public:
    virtual ~CodecNegotiator()                                                    = default;
    virtual std::optional<Description> makeAnswer(const Description &offer) const = 0;
    virtual bool                       acceptsAnswer(const Description &offer, const Description &answer) const = 0;
};

// One initial offer/answer exchange for one content. Does not activate media,
// authorize capture, negotiate BUNDLE, or validate transport/security parameters.
// Current interoperability policy requires preserving offered payload IDs.
// A future remapping implementation must supply explicit directional PT maps.
class IRIS_EXPORT Negotiation {
public:
    enum class State { Empty, Offered, Accepted };
    enum class Result { Ok, WrongState, InvalidDescription, IncompatibleAnswer, UnsupportedMedia };

    Result setLocalOffer(const Description &);
    Result setRemoteOffer(const Description &, const CodecNegotiator &);
    // Commit an answer prepared asynchronously by the media backend. Both
    // descriptions are validated and snapshotted atomically; failure leaves the
    // negotiation empty.
    Result setRemoteOffer(const Description &offer, const Description &localAnswer);
    Result setRemoteAnswer(const Description &, const CodecNegotiator &);
    State  state() const { return state_; }
    // Detached value copies: opaque XML extensions do not retain parser-owned DOM handles.
    std::optional<Description> localDescription() const;
    std::optional<Description> remoteDescription() const;

private:
    State                      state_ = State::Empty;
    std::optional<Description> local_, remote_;
};

}
#endif
