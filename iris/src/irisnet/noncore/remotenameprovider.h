/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef IRISNET_REMOTENAMEPROVIDER_H
#define IRISNET_REMOTENAMEPROVIDER_H

#include <iris/irisnet/corelib/irisnetplugin.h>

#include <QNetworkProxy>
#include <QUrl>

#include <memory>

namespace XMPP {
/*!
   \brief Resolves names with a remote DNS server reached over a TCP stream

   The server is given as a URL:
   - \c tcp://1.1.1.1 for DNS over TCP (RFC 7766), port 53 by default;
   - \c https://cloudflare-dns.com/dns-query for DNS over HTTPS (RFC 8484), port 443 by default. The server
     certificate is verified against the system trust store.

   With a SOCKS5 or HTTP CONNECT \a proxy the connection to the DNS server is tunnelled through the proxy by the
   same SocksClient and HttpConnect transports XMPP connections use. Names are then resolved from the proxy's
   network and no DNS query leaves the local host, not even one for the DNS server's own host name. Without a
   proxy the server is reached directly.

   Install the provider with NameResolver::setNameProvider() or ServiceResolver::setNameProvider(). Lookups share
   one connection, which is closed after a few idle seconds or when the provider is destroyed. Long-lived lookups
   are not supported.

   JDNS builds the queries and parses the replies. Without it, see isSupported(), every lookup fails.
*/
class IRISNET_EXPORT RemoteNameProvider : public NameProvider {
    Q_OBJECT
public:
    explicit RemoteNameProvider(const QUrl &server, const QNetworkProxy &proxy = QNetworkProxy(QNetworkProxy::NoProxy),
                                QObject *parent = nullptr);
    ~RemoteNameProvider() override;

    //! Returns true if Iris was built with JDNS, which remote lookups need.
    static bool isSupported();

    /*!
       \brief Validate a DNS-over-TCP or DNS-over-HTTPS server URL

       Accepts \c tcp://host[:port] (without query parameters or a path
       other than \c /) or \c https://host[:port]/path?query.
       Rejects credentials, fragments, port zero, and unsupported schemes.
       In particular, unencrypted HTTP is never used for DoH.
    */
    static bool isValidServer(const QUrl &server);

    /*!
       Returns the transport error of the last failed connection to the DNS server, a ByteStream error code such
       as SocksClient::ErrProxyAuth or BSocket::ErrConnectionRefused, or ByteStream::ErrOk once a connection
       succeeded.
    */
    int connectionError() const;

    //! Returns a description of the last failure, or an empty string.
    QString errorString() const;

    bool supportsSingle() const override;
    bool supportsLongLived() const override;
    bool supportsRecordType(int type) const override;
    int  resolve_start(const QByteArray &name, int qType, bool longLived) override;
    void resolve_stop(int id) override;

private:
    class Private;
    friend class Private;
    std::unique_ptr<Private> d;
};
} // namespace XMPP

#endif // IRISNET_REMOTENAMEPROVIDER_H
