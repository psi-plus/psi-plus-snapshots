#include <iris/irisnet/corelib/addressresolver.h>
#include <iris/irisnet/corelib/irisnetplugin.h>
#include <iris/irisnet/corelib/objectsession.h>
#include <iris/irisnet/noncore/cutestuff/bsocket.h>
#include <iris/irisnet/noncore/cutestuff/httpconnect.h>
#include <iris/irisnet/noncore/cutestuff/httppoll.h>
#include <iris/irisnet/noncore/cutestuff/socks.h>
#include <iris/irisnet/noncore/dtls.h>
#include <iris/irisnet/noncore/ice176.h>
#include <iris/irisnet/noncore/iceabstractstundisco.h>
#include <iris/irisnet/noncore/iceagent.h>
#include <iris/irisnet/noncore/icetransport.h>
#include <iris/irisnet/noncore/legacy/ndns.h>
#include <iris/irisnet/noncore/legacy/srvresolver.h>
#include <iris/irisnet/noncore/processquit.h>
#include <iris/irisnet/noncore/stunallocate.h>
#include <iris/irisnet/noncore/stunbinding.h>
#include <iris/irisnet/noncore/stunmessage.h>
#include <iris/irisnet/noncore/stuntransaction.h>
#include <iris/irisnet/noncore/tcpportreserver.h>
#include <iris/irisnet/noncore/turnclient.h>
#include <iris/irisnet/noncore/udpportreserver.h>

#include <typeinfo>

// Volatile stores retain symbol references in Release builds without invoking
// networking, installing signal handlers or requiring a QCA provider.
template <typename T> void requireSymbol(T value)
{
    T volatile symbol = value;
    (void)symbol;
}

template <typename T> void requireQObject()
{
    requireSymbol(&T::staticMetaObject);
    requireSymbol(&typeid(T));
}

int main()
{
    requireQObject<ByteStream>();
    requireQObject<BSocket>();
    requireSymbol(&BSocket::address);
    requireQObject<HttpConnect>();
    requireQObject<HttpPoll>();
    requireQObject<SocksUDP>();
    requireQObject<SocksClient>();
    requireQObject<SocksServer>();
    requireQObject<NDns>();
    requireQObject<SrvResolver>();

    using namespace XMPP;
    requireSymbol(&irisNetCleanup);
    requireSymbol(&irisNetSetPluginPaths);
    requireQObject<AddressResolver>();
    requireQObject<NetAvailability>();
    requireQObject<NetInterface>();
    requireQObject<NetInterfaceManager>();
    requireQObject<NameResolver>();
    requireQObject<ServiceBrowser>();
    requireQObject<ServiceResolver>();
    requireQObject<ServiceLocalPublisher>();
    requireQObject<IrisNetProvider>();
    requireQObject<NetInterfaceProvider>();
    requireQObject<NetGatewayProvider>();
    requireQObject<NetAvailabilityProvider>();
    requireQObject<NameProvider>();
    requireQObject<ServiceProvider>();
    requireQObject<ObjectSession>();
    requireSymbol(&ObjectSessionWatcher::isValid);
    requireQObject<Dtls>();
    requireSymbol(&Dtls::supportedSRTPProfiles);
    requireQObject<Ice176>();
    requireQObject<AbstractStunDisco>();
    requireQObject<IceAgent>();
    requireSymbol(&IceAgent::randomCredential);
    requireQObject<IceComponent>();
    requireQObject<IceTransport>();
    requireQObject<ProcessQuit>();
    requireSymbol(&ProcessQuit::instance);
    requireQObject<StunAllocate>();
    requireQObject<StunBinding>();
    requireQObject<StunTransaction>();
    requireQObject<StunTransactionPool>();
    requireQObject<TcpPortServer>();
    requireQObject<TcpPortDiscoverer>();
    requireQObject<TcpPortScope>();
    requireQObject<TcpPortReserver>();
    requireQObject<TurnClient>();
    requireQObject<UdpPortReserver>();

    // Non-QObject API must also survive static archive extraction.
    requireSymbol(&NetNames::cleanup);
    requireSymbol(&NameRecord::type);
    requireSymbol(&ServiceInstance::name);
    requireSymbol(&WeightedNameRecordList::isEmpty);
    StunMessage message;
    StunMessage copy(message);
    return copy.isNull() ? 0 : 1;
}
