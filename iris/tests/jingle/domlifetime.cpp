// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-ice-udp.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDomDocument>
#include <iris/jingle-pub.h>
#include <iris/jingle-rtp-description.h>

namespace J = XMPP::Jingle;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void churnDom()
{
    for (int i = 0; i < 128; ++i) {
        QDomDocument doc;
        auto root = doc.createElementNS(QStringLiteral("urn:iris:churn"), QStringLiteral("root"));
        root.setAttribute(QStringLiteral("iteration"), i);
        for (int j = 0; j < 8; ++j)
            root.appendChild(doc.createElement(QStringLiteral("child")));
        doc.appendChild(root);
    }
}

static J::RTP::Description rtpFromTemporaryDocument()
{
    QDomDocument doc;
    check(doc.setContent(
              QStringLiteral("<description xmlns='urn:xmpp:jingle:apps:rtp:1' media='audio'>"
                             "<payload-type id='111' name='opus' clockrate='48000' channels='2'/>"
                             "<rtcp-mux/><future xmlns='urn:iris:future' value='kept'/></description>"),
              true),
          "invalid RTP lifetime fixture");
    auto parsed = J::RTP::Description::fromXml(doc.documentElement());
    check(parsed.has_value(), "failed to parse RTP lifetime fixture");
    return std::move(*parsed);
}

static J::ICE::UdpTransportDescription iceUdpFromTemporaryDocument()
{
    QDomDocument doc;
    check(doc.setContent(
              QStringLiteral("<transport xmlns='urn:xmpp:jingle:transports:ice-udp:1' pwd='secret' ufrag='frag'>"
                             "<fingerprint xmlns='urn:xmpp:jingle:apps:dtls:0' hash='sha-256' "
                             "setup='actpass'>AA:BB</fingerprint></transport>"),
              true),
          "invalid ICE-UDP lifetime fixture");
    auto parsed = J::ICE::UdpTransportCodec::fromXml(doc.documentElement());
    check(parsed.has_value(), "failed to parse ICE-UDP lifetime fixture");
    return std::move(*parsed);
}

static J::JinglePub publicationFromTemporaryDocument()
{
    QDomDocument doc;
    check(doc.setContent(
              QStringLiteral("<jinglepub xmlns='urn:xmpp:jinglepub:1' from='publisher@example.test/device' id='pub'>"
                             "<description xmlns='urn:iris:published:media'/></jinglepub>"),
              true),
          "invalid JinglePub lifetime fixture");
    J::JinglePub publication(doc.documentElement());
    check(publication.isValid(), "failed to parse JinglePub lifetime fixture");
    return publication;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    auto rtp = rtpFromTemporaryDocument();
    churnDom();
    QDomDocument rtpOut;
    auto rtpXml = rtp.toXml(rtpOut);
    check(!rtpXml.isNull() && !rtpXml.firstChildElement(QStringLiteral("future")).isNull(),
          "RTP opaque extension depended on source QDomDocument lifetime");

    auto ice = iceUdpFromTemporaryDocument();
    churnDom();
    QDomDocument iceOut;
    auto iceXml = J::ICE::UdpTransportCodec::toXml(iceOut, ice);
    check(!iceXml.isNull() && !iceXml.firstChildElement(QStringLiteral("fingerprint")).isNull(),
          "ICE-UDP extension depended on source QDomDocument lifetime");

    auto publication = publicationFromTemporaryDocument();
    churnDom();
    QDomDocument publicationOut;
    auto publicationXml = publication.toXml(&publicationOut);
    check(!publicationXml.isNull()
              && !publicationXml.firstChildElement(QStringLiteral("description")).isNull(),
          "JinglePub description depended on source QDomDocument lifetime");

    J::JinglePub generated;
    generated.setFrom(XMPP::Jid(QStringLiteral("publisher@example.test/device")));
    generated.setId(QStringLiteral("generated"));
    generated.addDescription(QStringLiteral("urn:iris:generated:media"));
    churnDom();
    QDomDocument generatedOut;
    check(!generated.toXml(&generatedOut).isNull(),
          "JinglePub generated description depended on a local QDomDocument");

    auto detached = publication;
    detached.addDescription(QStringLiteral("urn:iris:copy"));
    churnDom();
    QDomDocument originalOut;
    QDomDocument detachedOut;
    const auto originalXml = publication.toXml(&originalOut);
    const auto detachedXml = detached.toXml(&detachedOut);
    check(!originalXml.isNull() && !detachedXml.isNull()
              && originalXml.elementsByTagName(QStringLiteral("description")).size() == 1
              && detachedXml.elementsByTagName(QStringLiteral("description")).size() == 2,
          "JinglePub copy-on-write broke description DOM ownership");

    qInfo("Jingle DOM lifetime regressions passed");
}
