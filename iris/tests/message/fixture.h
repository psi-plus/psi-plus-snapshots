// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <iris/xmpp.h>
#include <iris/xmpp_client.h>
#include <iris/xmpp_clientstream.h>

#include <QtGlobal>

namespace MessageTest {

inline void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

class OfflineConnector final : public XMPP::Connector {
public:
    void setOptHostPort(const QString &, quint16) override { }
    void connectToServer(const QString &) override { }
    ByteStream *stream() const override { return nullptr; }
    void done() override { }
};

class RecordingStream final : public XMPP::ClientStream {
public:
    explicit RecordingStream(XMPP::Connector *connector) : ClientStream(connector) { }
};

struct Endpoint {
    OfflineConnector connector;
    RecordingStream stream { &connector };
    XMPP::Client client;

    Endpoint()
    {
        client.start(QStringLiteral("example.test"), QStringLiteral("alice"), QString(),
                     QStringLiteral("desktop"));
        client.connectToServer(&stream, XMPP::Jid(QStringLiteral("alice@example.test/desktop")));
    }
};

} // namespace MessageTest
