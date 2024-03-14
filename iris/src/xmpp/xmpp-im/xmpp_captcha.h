/*
 * Copyright (C) 2016  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_CAPTCHA_H
#define XMPP_CAPTCHA_H

#include "xmpp/jid/jid.h"
#include "xmpp_url.h"

#include <QDateTime>

namespace XMPP {
class CaptchaChallengePrivate;
class Message;
class XData;

class CaptchaChallenge {
public:
    enum Result { Passed, Unavailable, NotAcceptable };

    enum State { New, Success, Fail };

    static const int Timeout = 120; // secs

    CaptchaChallenge();
    CaptchaChallenge(const Message &);
    CaptchaChallenge(const CaptchaChallenge &);
    ~CaptchaChallenge();

    CaptchaChallenge &operator=(const CaptchaChallenge &);

    bool           isValid() const;
    const Jid     &offendedJid() const;
    const Jid     &arbiter() const;
    const XData   &form() const;
    QString        explanation() const;
    const UrlList &urls() const;
    State          state() const;

    Result validateResponse(const XData &);

private:
    friend class CaptchaChallengePrivate;
    QSharedDataPointer<CaptchaChallengePrivate> d;
};
} // namespace XMPP

#endif // XMPP_CAPTCHA_H
