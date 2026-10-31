// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <iris/jingle-session.h>

namespace XMPP { namespace Jingle {

    // Keep the production declarations intact: MSVC encodes member access in symbol names.
    struct SessionTestAccess {
        static bool updateFromXml(Session &session, Action action, const QDomElement &element,
                                  std::function<void()> *afterReply = nullptr)
        {
            return session.updateFromXml(action, element, afterReply);
        }
        static bool incomingInitiate(Session &session, const Jingle &jingle, const QDomElement &element)
        {
            return session.incomingInitiate(jingle, element);
        }
        static std::optional<QList<ContentGroup>> parseGroupings(const QDomElement &element)
        {
            return Session::parseGroupings(element);
        }
        static bool validBundleAnswer(const QList<ContentGroup> &offer, const QList<ContentGroup> &answer)
        {
            return Session::validBundleAnswer(offer, answer);
        }
        static bool validLocalGroupings(const Session &session) { return session.validLocalGroupings(); }
    };

}}
