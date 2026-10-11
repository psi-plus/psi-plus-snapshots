// SPDX-License-Identifier: LGPL-2.1-or-later
// Regression coverage for malformed and future XMPP error conditions.
// Uses the real XML stream parser and CoreProtocol state machine without a network.
#include "../../src/xmpp/xmpp-core/protocol.h"

#include <QByteArray>
#include <QCoreApplication>
#include <cstdlib>
#include <variant>

using namespace XMPP;

static void check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
        std::exit(1);
    }
}

static QByteArray streamOpen()
{
    return QByteArrayLiteral(
        "<stream:stream xmlns:stream='http://etherx.jabber.org/streams' "
        "xmlns='jabber:client' from='example.test' version='1.0'>");
}

static void beginClient(CoreProtocol &protocol, const QByteArray &features)
{
    protocol.startClientOut(Jid(QStringLiteral("alice@example.test/desktop")), false, true, true, false);
    check(protocol.processStep() && protocol.event == XmlProtocol::ESend, "Failed to send stream header");
    protocol.addIncomingData(streamOpen() + features);
    check(protocol.processStep() && protocol.event == XmlProtocol::ERecvOpen, "Failed to parse stream header");
    check(protocol.processStep() && protocol.event == BasicProtocol::EFeatures, "Failed to parse stream features");
}

static BasicProtocol::SASLCond saslFailure(const QByteArray &failure,
                                           QHash<QString, QString> *localizedDescriptions = nullptr)
{
    CoreProtocol protocol;
    beginClient(protocol, QByteArrayLiteral(
        "<stream:features><mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
        "<mechanism>PLAIN</mechanism></mechanisms></stream:features>"));

    check(!protocol.processStep() && protocol.need == BasicProtocol::NSASLFirst,
          "SASL mechanism was not requested");
    protocol.setSASLFirst(QStringLiteral("PLAIN"), {});
    check(protocol.processStep() && protocol.event == XmlProtocol::ESend, "SASL auth was not sent");
    protocol.addIncomingData(failure);
    check(protocol.processStep() && protocol.event == XmlProtocol::EError, "Missing SASL failure event");
    check(protocol.errorCode == BasicProtocol::ErrAuth, "Wrong SASL error code");
    check(protocol.errCond.has_value(), "SASL failure left errCond disengaged");
    if (localizedDescriptions)
        *localizedDescriptions = protocol.errLangText;
    const auto *cond = std::get_if<BasicProtocol::SASLCond>(&*protocol.errCond);
    check(cond != nullptr, "Wrong SASL error condition type");
    return *cond;
}

static std::optional<BasicProtocol::ErrorCond> bindingFailure(const QByteArray &error)
{
    CoreProtocol protocol;
    // Simulate an authenticated stream; no QCA plugin or network is needed.
    protocol.setSASLAuthed();
    beginClient(protocol, QByteArrayLiteral(
        "<stream:features><bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
        "</stream:features>"));
    check(protocol.processStep() && protocol.event == XmlProtocol::ESend, "Binding request was not sent");
    protocol.addIncomingData(error);
    check(protocol.processStep() && protocol.event == XmlProtocol::EError, "Missing binding error event");
    check(protocol.errorCode == BasicProtocol::ErrBind, "Wrong binding error code");
    return protocol.errCond;
}

static void testSasl()
{
    using Cond = BasicProtocol::SASLCond;
    const QByteArray ns = QByteArrayLiteral("urn:ietf:params:xml:ns:xmpp-sasl");
    check(saslFailure("<failure xmlns='" + ns + "'><not-authorized/></failure>") == Cond::NotAuthorized,
          "Known SASL condition changed");
    check(saslFailure("<s:failure xmlns:s='" + ns + "'><s:temporary-auth-failure/></s:failure>")
              == Cond::TemporaryAuthFailure,
          "Prefixed SASL condition not recognized");
    check(saslFailure("<failure xmlns='" + ns + "'><text>Try later</text>"
                      "<account-disabled/></failure>") == Cond::AccountDisabled,
          "Text preceding SASL condition masked condition");

    // Exact SASL failure payload from a real Psi crash. The server
    // violates RFC 6120's condition-before-text ordering; that must not
    // cause the protocol or ClientStream to dereference an empty optional.
    QHash<QString, QString> descriptions;
    check(saslFailure(QByteArrayLiteral(
                          "<failure xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                          "<text xml:lang='en'>Invalid username or password</text>"
                          "<not-authorized/></failure>"),
                      &descriptions) == Cond::NotAuthorized,
          "Real-world text-first SASL failure was not recognized");
    check(descriptions.value(QStringLiteral("en")) == QStringLiteral("Invalid username or password"),
          "SASL failure lost xml:lang on its description");
    check(saslFailure("<failure xmlns='" + ns + "'><new-condition/></failure>") == Cond::NotAuthorized,
          "Unknown SASL condition must be treated as not-authorized (RFC 6120)");
    check(saslFailure("<failure xmlns='" + ns + "'><text>Only text</text></failure>")
              == Cond::NotAuthorized,
          "Text-only SASL failure was not handled");
    check(saslFailure("<failure xmlns='" + ns + "'/>") == Cond::NotAuthorized,
          "Empty SASL failure was not handled");
}

static void testBinding()
{
    const QByteArray errorOpen = QByteArrayLiteral("<iq id='bind_1' type='error'><error type='cancel'>");
    const QByteArray errorClose = QByteArrayLiteral("</error></iq>");
    const QByteArray ns = QByteArrayLiteral("urn:ietf:params:xml:ns:xmpp-stanzas");

    const auto conflict = bindingFailure(errorOpen + "<text xmlns='" + ns + "'>Resource busy</text>"
                                                "<conflict xmlns='" + ns + "'/>" + errorClose);
    check(conflict.has_value() && std::holds_alternative<BasicProtocol::BindCond>(*conflict)
              && std::get<BasicProtocol::BindCond>(*conflict) == BasicProtocol::BindCond::BindConflict,
          "Text preceding binding condition masked conflict");

    const auto unknown = bindingFailure(errorOpen + "<service-unavailable xmlns='" + ns + "'/>" + errorClose);
    check(!unknown.has_value(), "Unknown binding condition unexpectedly classified");

    const auto missing = bindingFailure(errorOpen + "<text xmlns='" + ns + "'>No condition</text>" + errorClose);
    check(!missing.has_value(), "Text-only binding error unexpectedly classified");
}

static void testStream()
{
    CoreProtocol protocol;
    protocol.startClientOut(Jid(QStringLiteral("alice@example.test/desktop")), false, true, true, false);
    check(protocol.processStep() && protocol.event == XmlProtocol::ESend, "Failed to send stream header");
    protocol.addIncomingData(streamOpen()
                             + "<stream:error><s:future-condition "
                               "xmlns:s='urn:ietf:params:xml:ns:xmpp-streams'/>"
                               "<text xmlns='urn:ietf:params:xml:ns:xmpp-streams' "
                               "xml:lang='en'>Unknown failure</text></stream:error>");
    check(protocol.processStep() && protocol.event == XmlProtocol::ERecvOpen, "Failed to parse stream header");
    check(protocol.processStep() && protocol.event == XmlProtocol::EError, "Missing stream error event");
    check(protocol.errorCode == BasicProtocol::ErrStream, "Wrong stream error code");
    check(protocol.errCond.has_value()
              && std::holds_alternative<BasicProtocol::StreamCond>(*protocol.errCond)
              && std::get<BasicProtocol::StreamCond>(*protocol.errCond)
                  == BasicProtocol::StreamCond::UndefinedCondition,
          "Unknown stream condition was not normalized");
    check(protocol.errLangText.value(QStringLiteral("en")) == QStringLiteral("Unknown failure"),
          "Stream error lost xml:lang on its description");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testSasl();
    testBinding();
    testStream();
    return 0;
}
