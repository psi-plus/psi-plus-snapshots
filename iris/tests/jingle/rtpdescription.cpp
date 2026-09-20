#include <QCoreApplication>
#include <QDebug>
#include <iris/jingle-rtp-description.h>

using XMPP::Jingle::Origin;
using XMPP::Jingle::RTP::Description;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static std::optional<Description> parseXml(const QString &xml, bool advisory = false)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "invalid test XML");
    return Description::fromXml(doc.documentElement(), advisory);
}

static std::optional<Description> parse(const QString &body, bool advisory = false)
{
    return parseXml("<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>" + body + "</description>",
                    advisory);
}

static QString opaqueNamespace(const QByteArray &xml)
{
    QDomDocument doc;
    check(doc.setContent(xml, true), "invalid opaque extension XML");
    return doc.documentElement().namespaceURI();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto description = parse("<rtcp-fb xmlns='urn:xmpp:jingle:apps:rtp:rtcp-fb:0' type='nack' subtype='pli'>"
                                   "<parameter name='scope'/></rtcp-fb>"
                                   "<rtcp-fb-trr-int xmlns='urn:xmpp:jingle:apps:rtp:rtcp-fb:0' value='0'/>"
                                   "<payload-type id='111' name='opus' clockrate='48000' channels='2'>"
                                   "<parameter name='minptime' value='10'/><parameter name='useinbandfec' value='1'/>"
                                   "<rtcp-fb xmlns='urn:xmpp:jingle:apps:rtp:rtcp-fb:0' type='transport-cc'/>"
                                   "<rtcp-fb-trr-int xmlns='urn:xmpp:jingle:apps:rtp:rtcp-fb:0' value='100'/>"
                                   "</payload-type><payload-type id='0'/><rtcp-mux/>"
                                   "<ssrc-group xmlns='urn:xmpp:jingle:apps:rtp:ssma:0' semantics='FID'>"
                                   "<source ssrc='100'/><source ssrc='101'/></ssrc-group>"
                                   "<source xmlns='urn:xmpp:jingle:apps:rtp:ssma:0' ssrc='100'>"
                                   "<parameter name='cname' value='voice'/><parameter name='msid'/></source>"
                                   "<source xmlns='urn:xmpp:jingle:apps:rtp:ssma:0' ssrc='101'/>"
                                   "<rtp-hdrext xmlns='urn:xmpp:jingle:apps:rtp:rtp-hdrext:0' id='1' "
                                   "uri='urn:ietf:params:rtp-hdrext:sdes:mid' senders='initiator'>"
                                   "<parameter name='mode'/></rtp-hdrext>"
                                   "<extmap-allow-mixed xmlns='urn:xmpp:jingle:apps:rtp:rtp-hdrext:0'/>"
                                   "<future xmlns='urn:example:rtp:future' value='kept'/>");
    check(description && description->payloads.size() == 2, "valid description rejected");
    check(description->payloads.first().id == 111 && description->payloads.last().id == 0, "codec preference lost");
    check(description->payloads.first().clockrate == 48000 && description->payloads.first().channels == 2,
          "Opus parameters lost");
    check(description->payloads.last().channels.value_or(1) == 1, "default channel count changed");
    check(description->feedback.size() == 1 && description->feedback.first().type == QStringLiteral("nack")
              && description->feedback.first().subtype == QStringLiteral("pli")
              && description->feedback.first().parameters.size() == 1
              && !description->feedback.first().parameters.first().value.has_value()
              && description->feedbackTrrInt == 0,
          "description RTCP feedback was not typed");
    check(description->payloads.first().feedback.size() == 1
              && description->payloads.first().feedback.first().type == QStringLiteral("transport-cc")
              && description->payloads.first().feedbackTrrInt == 100,
          "payload RTCP feedback was not typed");
    check(description->headerExtensions.size() == 1 && description->headerExtensions.first().id == 1
              && description->headerExtensions.first().senders == Origin::Initiator
              && description->headerExtensions.first().parameters.size() == 1 && description->extmapAllowMixed,
          "RTP header extension was not typed");
    check(description->sources.size() == 2 && description->sources.first().ssrc == 100
              && description->sources.first().parameters.size() == 2
              && !description->sources.first().parameters.last().value.has_value(),
          "SSMA sources were not typed");
    check(description->sourceGroups.size() == 1
              && description->sourceGroups.first().sources == QList<quint32>({ 100, 101 }),
          "SSMA source group was not typed");
    check(description->extensions.size() == 1 && description->payloads.first().extensions.isEmpty(),
          "known RTP extensions stayed opaque or unknown extension was lost");

    QDomDocument doc;
    doc.appendChild(description->toXml(doc));
    const auto roundtrip = parseXml(doc.toString());
    check(roundtrip && roundtrip->rtcpMux && roundtrip->feedback.size() == 1
              && roundtrip->payloads.first().feedback.size() == 1 && roundtrip->headerExtensions.size() == 1
              && roundtrip->sources.size() == 2 && roundtrip->sourceGroups.size() == 1
              && roundtrip->extensions.size() == 1
              && opaqueNamespace(roundtrip->extensions.first()) == "urn:example:rtp:future",
          "typed or opaque extensions lost on roundtrip");

    const auto boundaryIds = parse(
        "<payload-type id='0'/>"
        "<rtp-hdrext xmlns='urn:xmpp:jingle:apps:rtp:rtp-hdrext:0' id='256' uri='urn:test:one'/>"
        "<rtp-hdrext xmlns='urn:xmpp:jingle:apps:rtp:rtp-hdrext:0' id='4096' uri='urn:test:two' senders='none'/>");
    check(boundaryIds && boundaryIds->headerExtensions.size() == 2
              && boundaryIds->headerExtensions.last().senders == Origin::None,
          "valid header-extension boundary id or senders=none rejected");

    check(!parse("<payload-type id='128' name='opus'/>") && !parse("<payload-type id='-1'/>")
              && !parse("<payload-type id='0'/><payload-type id='0'/>") && !parse("<payload-type id='111'/>")
              && !parse("<payload-type id='0' clockrate='4294967296'/>")
              && !parse("<payload-type id='0' channels='0'/>") && !parse(""),
          "invalid payload accepted");
    check(!parse("<payload-type id='0'><parameter name='x' value='1'/><parameter name='x' value='2'/></payload-type>"),
          "ambiguous fmtp accepted");

    const QString fbNs = QStringLiteral("urn:xmpp:jingle:apps:rtp:rtcp-fb:0");
    check(!parse("<payload-type id='0'/><rtcp-fb xmlns='" + fbNs + "'/>")
              && !parse("<payload-type id='0'/><rtcp-fb xmlns='" + fbNs + "' type='nack'><bad/></rtcp-fb>")
              && !parse("<payload-type id='0'/><rtcp-fb-trr-int xmlns='" + fbNs + "'/>")
              && !parse("<payload-type id='0'/><rtcp-fb-trr-int xmlns='" + fbNs + "' value='-1'/>")
              && !parse("<payload-type id='0'/><rtcp-fb-trr-int xmlns='" + fbNs
                        + "' value='1'/><rtcp-fb-trr-int xmlns='" + fbNs + "' value='2'/>")
              && !parse("<payload-type id='0'><rtcp-fb xmlns='" + fbNs
                        + "' type='nack'><parameter value='x'/></rtcp-fb></payload-type>"),
          "malformed RTCP feedback accepted");

    const QString hdrNs = QStringLiteral("urn:xmpp:jingle:apps:rtp:rtp-hdrext:0");
    for (const auto &id :
         { QStringLiteral("0"), QStringLiteral("257"), QStringLiteral("4095"), QStringLiteral("4352") }) {
        check(!parse("<payload-type id='0'/><rtp-hdrext xmlns='" + hdrNs + "' id='" + id + "' uri='urn:test'/>"),
              "invalid RTP header-extension id accepted");
    }
    check(!parse("<payload-type id='0'/><rtp-hdrext xmlns='" + hdrNs + "' id='1'/>")
              && !parse("<payload-type id='0'/><rtp-hdrext xmlns='" + hdrNs
                        + "' id='1' uri='urn:test' senders='invalid'/>")
              && !parse("<payload-type id='0'/><extmap-allow-mixed xmlns='" + hdrNs + "'/><extmap-allow-mixed xmlns='"
                        + hdrNs + "'/>")
              && !parse("<payload-type id='0'/><rtp-hdrext xmlns='" + hdrNs
                        + "' id='1' uri='urn:test'><parameter/></rtp-hdrext>"),
          "malformed RTP header extension accepted");

    const QString ssmaNs = QStringLiteral("urn:xmpp:jingle:apps:rtp:ssma:0");
    check(!parse("<payload-type id='0'/><source xmlns='" + ssmaNs + "'/>")
              && !parse("<payload-type id='0'/><source xmlns='" + ssmaNs + "' ssrc='4294967296'/>")
              && !parse("<payload-type id='0'/><source xmlns='" + ssmaNs + "' ssrc='1'/><source xmlns='" + ssmaNs
                        + "' ssrc='1'/>")
              && !parse("<payload-type id='0'/><source xmlns='" + ssmaNs + "' ssrc='1'><parameter value='x'/></source>")
              && !parse("<payload-type id='0'/><ssrc-group xmlns='" + ssmaNs + "'><source ssrc='1'/></ssrc-group>")
              && !parse("<payload-type id='0'/><ssrc-group xmlns='" + ssmaNs
                        + "' semantics='FID'><source ssrc='1'/><source ssrc='1'/></ssrc-group>"),
          "malformed source-specific media attributes accepted");

    const auto hint = parse("<payload-type id='111'><parameter name='x' value='1'/></payload-type>", true);
    check(hint && !hint->payloads.first().channels && !hint->payloads.first().clockrate,
          "advisory update invented omitted parameters");
    qInfo("RTP description regressions passed");
}
