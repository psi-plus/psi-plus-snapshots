#include <QCoreApplication>
#include <iris/xmpp-im/jingle-ibb.h>
#include <iris/jingle-ice.h>
#include <iris/jingle-nstransportslist.h>
#include <iris/xmpp-im/jingle-s5b.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_caps.h>
#include <iris/xmpp_client.h>

using namespace XMPP;
using namespace XMPP::Jingle;

static void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

static void setPeerFeatures(Client &client, const Jid &peer, const QStringList &features)
{
    DiscoItem disco;
    disco.setJid(peer);
    disco.setNode(QStringLiteral("urn:iris:test:jingle-transport-selection"));
    disco.setFeatures(Features(features));

    const CapsSpec caps(disco);
    CapsRegistry::instance()->registerCaps(caps, disco);
    client.capsManager()->updateCaps(peer, caps);
}

static QString selectTransport(Session &session, const QStringList &transports)
{
    // NSTransportsList prefers the last namespace in the list.
    NSTransportsList selector(&session, transports);
    const auto       transport = selector.getNextTransport();
    return transport ? transport->pad()->ns() : QString();
}

static QString selectIceProfile(Session &session)
{
    return selectTransport(session, { ICE::NS_ICE_UDP, ICE::NS });
}

static bool isIceTransport(const QString &ns) { return ns == ICE::NS || ns == ICE::NS_ICE_UDP; }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    TcpPortReserver reserver;
    Client          client;
    client.setTcpPortReserver(&reserver);

    const Jid peer(QStringLiteral("peer@example.org/device"));
    Session   session(client.jingleManager(), peer);

    setPeerFeatures(client, peer, { ICE::NS, ICE::NS_ICE_UDP });
    check(selectIceProfile(session) == ICE::NS, "ice:0 was not preferred when both profiles were available");

    setPeerFeatures(client, peer, { ICE::NS_ICE_UDP });
    check(selectIceProfile(session) == ICE::NS_ICE_UDP, "ice-udp:1-only peer was rejected");

    setPeerFeatures(client, peer, { ICE::NS });
    check(selectIceProfile(session) == ICE::NS, "ice:0-only peer was rejected");

    setPeerFeatures(client, peer, {});
    check(selectIceProfile(session).isEmpty(), "transport selected without a matching peer capability");

    const TransportFeatures fileTransferRequirements
        = TransportFeature::Reliable | TransportFeature::Ordered | TransportFeature::DataOriented;
    const QStringList fileTransferTransports = client.jingleManager()->availableTransports(fileTransferRequirements);

    const int ibbIndex = fileTransferTransports.indexOf(IBB::NS);
    const int s5bIndex = fileTransferTransports.indexOf(S5B::NS);
    check(ibbIndex >= 0, "IBB does not satisfy Jingle file-transfer requirements");
    check(s5bIndex >= 0, "S5B does not satisfy Jingle file-transfer requirements");
    check(s5bIndex > ibbIndex, "S5B is not preferred over IBB for Jingle file transfer");

    const int iceIndex    = fileTransferTransports.indexOf(ICE::NS);
    const int iceUdpIndex = fileTransferTransports.indexOf(ICE::NS_ICE_UDP);
    if (iceIndex >= 0)
        check(iceIndex > s5bIndex, "ICE is not preferred over S5B for Jingle file transfer");
    if (iceUdpIndex >= 0)
        check(iceUdpIndex > s5bIndex, "ICE-UDP is not preferred over S5B for Jingle file transfer");

    QStringList allFileTransferCaps { IBB::NS, S5B::NS };
    if (iceIndex >= 0)
        allFileTransferCaps += ICE::NS;
    if (iceUdpIndex >= 0)
        allFileTransferCaps += ICE::NS_ICE_UDP;
    setPeerFeatures(client, peer, allFileTransferCaps);

    const QString preferredFileTransferTransport = selectTransport(session, fileTransferTransports);
    if (iceIndex >= 0 || iceUdpIndex >= 0)
        check(isIceTransport(preferredFileTransferTransport),
              "data-capable ICE was not selected first for Jingle file transfer");
    else
        check(preferredFileTransferTransport == S5B::NS,
              "S5B was not selected when ICE was not data-capable");

    setPeerFeatures(client, peer, { S5B::NS, IBB::NS });
    check(selectTransport(session, fileTransferTransports) == S5B::NS,
          "Jingle file transfer did not fall back from ICE to S5B");

    setPeerFeatures(client, peer, { IBB::NS });
    check(selectTransport(session, fileTransferTransports) == IBB::NS,
          "Jingle file transfer did not fall back from S5B to IBB");

    qInfo().noquote() << "Jingle file-transfer transport preference:"
                      << fileTransferTransports.join(QStringLiteral(" -> "));
    qInfo("Jingle transport selection regressions passed");
}
