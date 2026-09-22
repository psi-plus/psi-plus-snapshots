#include "jingle-ice-udp.h"
#include <QCoreApplication>
#include <QDebug>

using XMPP::Jingle::ICE::iceUdpToInternal;
using XMPP::Jingle::ICE::internalToIceUdp;
using XMPP::Jingle::ICE::NS_ICE_UDP;
using XMPP::Jingle::ICE::UdpTransportCodec;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static std::optional<XMPP::Jingle::ICE::UdpTransportDescription> parseXml(const QString &xml)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "invalid test XML");
    return UdpTransportCodec::fromXml(doc.documentElement());
}

static std::optional<XMPP::Jingle::ICE::UdpTransportDescription> parse(const QString &body)
{
    return parseXml(
        "<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1' pwd='secret' ufrag='frag'>" + body + "</transport>");
}

static QString opaqueNamespace(const QByteArray &xml)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "invalid opaque ICE-UDP extension");
    return doc.documentElement().namespaceURI();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const auto description
        = parse("<candidate component='1' foundation='1' generation='0' id='host1' ip='192.0.2.1' network='0' "
                "port='5000' priority='2130706431' protocol='udp' type='host'/>"
                "<candidate component='1' foundation='2' generation='0' id='relay1' ip='192.0.2.2' port='5001' "
                "priority='1677734911' protocol='udp' rel-addr='192.0.2.1' rel-port='5000' type='relay'/>"
                "<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='sha-256' setup='actpass'>AA:BB</fingerprint>");
    check(description && description->candidates.size() == 2, "valid ICE-UDP description rejected");
    check(description->candidates.first().network == 0 && description->candidates.last().network == -1,
          "optional network attribute changed");
    check(description->extensions.size() == 1
              && opaqueNamespace(description->extensions.first()) == QStringLiteral("urn:xmpp:jingle:apps:dtls:0"),
          "foreign transport extension lost");

    QDomDocument doc;
    const auto   transport = UdpTransportCodec::toXml(doc, *description);
    check(!transport.isNull() && transport.namespaceURI() == NS_ICE_UDP, "ICE-UDP serialization failed");
    doc.appendChild(transport);
    const auto roundtrip = parseXml(doc.toString());
    check(roundtrip && roundtrip->candidates.size() == 2 && roundtrip->extensions.size() == 1,
          "ICE-UDP roundtrip failed");

    QDomDocument internalDoc;
    const auto internal = iceUdpToInternal(internalDoc, transport, QStringLiteral("urn:xmpp:jingle:transports:ice:0"));
    check(!internal.isNull() && internal.namespaceURI() == QStringLiteral("urn:xmpp:jingle:transports:ice:0")
              && internal.firstChildElement(QStringLiteral("candidate")).namespaceURI().isEmpty(),
          "ICE-UDP to shared ICE conversion failed");
    check(!internal.firstChildElement(QStringLiteral("fingerprint")).isNull(), "DTLS extension lost in shared ICE");
    const auto bridged = internalToIceUdp(internalDoc, internal);
    check(!bridged.isNull() && bridged.namespaceURI() == NS_ICE_UDP
              && bridged.firstChildElement(QStringLiteral("candidate")).namespaceURI() == NS_ICE_UDP,
          "shared ICE to ICE-UDP conversion failed");
    const auto bridgedDescription = UdpTransportCodec::fromXml(bridged);
    check(bridgedDescription && bridgedDescription->candidates.size() == 2
              && bridgedDescription->extensions.size() == 1,
          "shared ICE bridge changed XEP-0176 semantics");

    auto internalWithCompletion = internal.cloneNode(true).toElement();
    internalWithCompletion.setAttribute(QStringLiteral("ice2"), QStringLiteral("true"));
    internalWithCompletion.appendChild(internalDoc.createElement(QStringLiteral("gathering-complete")));
    const auto withoutCompletion = internalToIceUdp(internalDoc, internalWithCompletion);
    check(!withoutCompletion.isNull()
              && withoutCompletion.firstChildElement(QStringLiteral("gathering-complete")).isNull(),
          "gathering-complete leaked through shared ICE bridge");
    check(!withoutCompletion.hasAttribute(QStringLiteral("ice2")),
          "RFC 8445 ice2 leaked into RFC 5245 XEP-0176 signaling");

    const auto selected = parse("<remote-candidate component='1' ip='192.0.2.10' port='6000'/>");
    check(selected && selected->remoteCandidate && selected->remoteCandidate->port == 6000,
          "valid remote-candidate rejected");

    check(parse("").has_value(), "empty ICE-UDP transport rejected");
    check(
        !parseXml(
            "<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1'><candidate component='1' foundation='1' "
            "generation='0' id='x' ip='192.0.2.1' port='5000' priority='1' protocol='udp' type='host'/></transport>"),
        "candidate without credentials accepted");
    check(!parse("<candidate component='0' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                 "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='256' id='x' ip='192.0.2.1' port='5000' "
                        "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='not-an-ip' port='5000' "
                        "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='0' "
                        "priority='1' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                        "priority='0' protocol='udp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                        "priority='1' protocol='tcp' type='host'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                        "priority='1' protocol='udp' type='bogus'/>")
              && !parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' network='256' "
                        "port='5000' priority='1' protocol='udp' type='host'/>"),
          "invalid ICE-UDP candidate accepted");

    check(!parse("<candidate component='1' foundation='1' generation='0' id='same' ip='192.0.2.1' port='5000' "
                 "priority='2' protocol='udp' type='host'/>"
                 "<candidate component='1' foundation='2' generation='0' id='same' ip='192.0.2.2' port='5001' "
                 "priority='1' protocol='udp' type='host'/>"),
          "duplicate candidate id accepted");
    check(!parse("<candidate component='1' foundation='1' generation='0' id='x' ip='192.0.2.1' port='5000' "
                 "priority='1' protocol='udp' type='host'/>"
                 "<remote-candidate component='1' ip='192.0.2.2' port='5001'/>"),
          "candidate and remote-candidate mixture accepted");
    check(!parse("<gathering-complete/>"), "XEP-0371 gathering-complete leaked into XEP-0176");
    check(!parseXml("<transport xmlns='urn:xmpp:jingle:transports:ice:0'/>").has_value(),
          "wrong ICE namespace accepted");

    qInfo("ICE-UDP codec regressions passed");
}
