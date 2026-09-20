// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QCoreApplication>
#include <QDebug>
#include <iris/jingle-rtp-negotiation.h>

namespace R = XMPP::Jingle::RTP;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static R::Description description()
{
    R::Description result;
    result.media   = QStringLiteral("audio");
    result.rtcpMux = true;
    R::PayloadType opus;
    opus.id        = 111;
    opus.name      = QStringLiteral("opus");
    opus.clockrate = 48000;
    opus.channels  = 2;
    result.payloads.append(opus);
    result.extensions.append(QByteArrayLiteral("<test xmlns=\"urn:iris:test\" value=\"original\"/>"));
    return result;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    auto offer  = description();
    auto answer = description();
    answer.ssrc = 42;
    R::Negotiation prepared;
    check(prepared.setRemoteOffer(offer, answer) == R::Negotiation::Result::Ok, "prepared answer was not committed");
    offer.extensions.first() = QByteArrayLiteral("<test xmlns=\"urn:iris:test\" value=\"changed\"/>");
    answer.payloads.first().name = QStringLiteral("changed");
    check(prepared.remoteDescription()->extensions.first().contains("value=\"original\""),
          "prepared offer snapshot leaked caller mutation");
    check(prepared.localDescription()->payloads.first().name == QStringLiteral("opus"),
          "prepared answer snapshot leaked caller mutation");
    check(prepared.setRemoteOffer(description(), description()) == R::Negotiation::Result::WrongState,
          "prepared answer replaced an accepted negotiation");

    auto invented                = description();
    invented.payloads.first().id = 112;
    R::Negotiation incompatible;
    check(incompatible.setRemoteOffer(description(), invented) == R::Negotiation::Result::IncompatibleAnswer,
          "prepared answer invented an unoffered payload");
    check(incompatible.state() == R::Negotiation::State::Empty && !incompatible.localDescription()
              && !incompatible.remoteDescription(),
          "failed prepared answer changed committed state");

    auto empty = description();
    empty.payloads.clear();
    R::Negotiation invalid;
    check(invalid.setRemoteOffer(empty, description()) == R::Negotiation::Result::InvalidDescription,
          "invalid prepared offer accepted");
    check(invalid.state() == R::Negotiation::State::Empty, "invalid prepared offer changed state");

    qInfo("RTP prepared-answer regressions passed");
}
