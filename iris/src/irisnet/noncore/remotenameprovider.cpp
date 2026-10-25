/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "remotenameprovider.h"

#include "bytestream.h"

#include <QSet>

#ifdef IRISNET_HAVE_JDNS
#include "bsocket.h"
#include "httpconnect.h"
#include "irisnet/corelib/jdnsrecord_p.h"
#include "socks.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QHostAddress>
#include <QTimer>
#include <QtCrypto>
#include <qjdns.h>
#include <qjdns_externaltransport.h>

#include <optional>
#endif

namespace XMPP {
#ifdef IRISNET_HAVE_JDNS
namespace {
    constexpr int TypeNull       = 10; // RFC 1035, not in QJDns::Type
    constexpr int MaxMessageSize = 65535;
    constexpr int MaxHeaderSize  = 16384;
    constexpr int RequestTimeout = 15000; // ms per DNS message, including the proxy and TLS handshakes
    constexpr int IdleTimeout    = 5000;  // ms an unused connection stays open

    void appendUInt16(QByteArray &out, quint16 value)
    {
        out.append(char(value >> 8));
        out.append(char(value & 0xff));
    }

    quint16 readUInt16(const QByteArray &data, int at)
    {
        return quint16((quint8(data[at]) << 8) | quint8(data[at + 1]));
    }

    // A DNS message starts with its 16-bit ID (RFC 1035 4.1.1).
    QByteArray withMessageId(QByteArray message, quint16 id)
    {
        if (message.size() >= 2) {
            message[0] = char(id >> 8);
            message[1] = char(id & 0xff);
        }
        return message;
    }

    // Incremental parser of the HTTP/1.1 response to one DNS over HTTPS request.
    class HttpReply {
    public:
        enum Result { NeedMore, Done, Invalid };

        int        status = 0;
        QByteArray body;
        bool       keepAlive = true;

        Result feed(const QByteArray &data)
        {
            buffer += data;
            if (!headersDone) {
                const int end = buffer.indexOf("\r\n\r\n");
                if (end < 0)
                    return buffer.size() > MaxHeaderSize ? Invalid : NeedMore;
                if (!parseHeaders(buffer.left(end)))
                    return Invalid;
                buffer.remove(0, end + 4);
                headersDone = true;
            }
            return parseBody(false);
        }

        // The server closed the connection.
        Result finish()
        {
            keepAlive = false;
            return headersDone ? parseBody(true) : Invalid;
        }

    private:
        QByteArray buffer;
        bool       headersDone   = false;
        bool       chunked       = false;
        qint64     contentLength = -1;

        bool parseHeaders(const QByteArray &head)
        {
            const QList<QByteArray> lines      = head.split('\n');
            const QByteArray        statusLine = lines.first().trimmed();
            const QList<QByteArray> parts      = statusLine.split(' ');
            bool                    ok         = false;
            if (!statusLine.startsWith("HTTP/1.") || parts.size() < 2)
                return false;
            status = parts[1].toInt(&ok);
            if (!ok)
                return false;
            keepAlive = !statusLine.startsWith("HTTP/1.0");
            for (int i = 1; i < lines.size(); ++i) {
                const int colon = lines[i].indexOf(':');
                if (colon <= 0)
                    continue;
                const QByteArray name  = lines[i].left(colon).trimmed().toLower();
                const QByteArray value = lines[i].mid(colon + 1).trimmed().toLower();
                if (name == "content-length") {
                    contentLength = value.toLongLong(&ok);
                    if (!ok || contentLength < 0 || contentLength > MaxMessageSize)
                        return false;
                } else if (name == "transfer-encoding") {
                    chunked = value.contains("chunked");
                } else if (name == "connection") {
                    if (value.contains("close"))
                        keepAlive = false;
                    else if (value.contains("keep-alive"))
                        keepAlive = true;
                }
            }
            return true;
        }

        Result parseBody(bool closed)
        {
            if (chunked) {
                while (true) {
                    const int lineEnd = buffer.indexOf("\r\n");
                    if (lineEnd < 0)
                        return closed ? Invalid : NeedMore;
                    bool         ok   = false;
                    const qint64 size = buffer.left(lineEnd).split(';').first().trimmed().toLongLong(&ok, 16);
                    if (!ok || size < 0 || body.size() + size > MaxMessageSize)
                        return Invalid;
                    if (size == 0) {
                        // the last chunk, optional trailer fields, and an empty line
                        int end = lineEnd + 4;
                        if (buffer.mid(lineEnd + 2, 2) != "\r\n") {
                            end = buffer.indexOf("\r\n\r\n", lineEnd + 2);
                            if (end < 0)
                                return closed ? Invalid : NeedMore;
                            end += 4;
                        }
                        if (buffer.size() < end)
                            return closed ? Invalid : NeedMore;
                        buffer.remove(0, end);
                        return Done;
                    }
                    if (buffer.size() < lineEnd + 2 + size + 2)
                        return closed ? Invalid : NeedMore;
                    body += buffer.mid(lineEnd + 2, int(size));
                    buffer.remove(0, int(lineEnd + 2 + size + 2));
                }
            }
            if (contentLength >= 0) {
                if (buffer.size() < contentLength)
                    return closed ? Invalid : NeedMore;
                body = buffer.left(int(contentLength));
                buffer.clear();
                return Done;
            }
            // without a length the body ends with the connection
            if (buffer.size() > MaxMessageSize)
                return Invalid;
            if (!closed)
                return NeedMore;
            body = buffer;
            return Done;
        }
    };

    QString streamErrorString(int code)
    {
        switch (code) {
        case SocksClient::ErrConnectionRefused:
            return RemoteNameProvider::tr("The connection to the DNS server was refused");
        case SocksClient::ErrHostNotFound:
            return RemoteNameProvider::tr("The DNS server host was not found");
        case SocksClient::ErrProxyConnect:
            return RemoteNameProvider::tr("Could not connect to the proxy");
        case SocksClient::ErrProxyNeg:
            return RemoteNameProvider::tr("The proxy refused the connection to the DNS server");
        case SocksClient::ErrProxyAuth:
            return RemoteNameProvider::tr("Proxy authentication failed");
        default:
            return RemoteNameProvider::tr("The connection to the DNS server failed");
        }
    }
} // namespace

/*
   JDNS builds the queries and parses the replies. This class carries the DNS
   messages JDNS hands to its external transport: through the proxy, framed for
   TCP or as HTTP requests over TLS.
*/
class RemoteNameProvider::Private : public QObject {
public:
    enum class Transport { Invalid, Tcp, Https };
    enum class Channel { Closed, Connecting, Handshaking, Ready };

    struct Lookup {
        int        query = -1; // JDNS query id
        QByteArray name;       // as asked, without the root label
        int        type = 0;
    };

    // A DNS message from JDNS, see QJDnsExternalTransport::queryRequested().
    struct Request {
        int        id;
        QByteArray message;
        quint16    wireId  = 0; // message ID on the current connection
        bool       sent    = false;
        bool       retried = false;
    };

    RemoteNameProvider *q;
    Transport           transport = Transport::Invalid;
    QString             host;
    quint16             port = 0;
    QByteArray          requestTarget; // DNS over HTTPS
    QByteArray          hostHeader;    // DNS over HTTPS
    QNetworkProxy       proxy;

    QJDns                  *dns          = nullptr;
    QJDnsExternalTransport *dnsTransport = nullptr; // owned by dns
    int                     nextId       = 0;
    QHash<int, Lookup>      lookups;   // by provider id
    QHash<int, int>         lookupIds; // provider id by JDNS query id
    QSet<int>               settled;   // finished lookups whose result is queued

    QList<Request>           requests; // pending and sent, in the order JDNS made them
    quint16                  nextWireId = 0;
    ByteStream              *stream     = nullptr;
    QCA::TLS                *tls        = nullptr;
    Channel                  channel    = Channel::Closed;
    QByteArray               tcpBuffer;
    std::optional<HttpReply> reply;            // to the DNS over HTTPS request in flight
    int                      httpRequest = -1; // -1 if no HTTP query (JDNS request ID 0 is valid)
    QTimer                   idleTimer;
    int                      connectionError = ByteStream::ErrOk;
    QString                  errorString;
    QDeadlineTimer           unreachable { 0 }; //!< server or proxy just failed: no reconnects until then

    Private(RemoteNameProvider *q, const QUrl &server, const QNetworkProxy &proxy) : q(q), proxy(proxy)
    {
        if (isValidServer(server)) {
            // ACE form: the proxy, TLS SNI and certificate check all see the same name
            host = server.host(QUrl::FullyEncoded);
            if (server.scheme() == QLatin1String("tcp")) {
                transport = Transport::Tcp;
                port      = quint16(server.port(53));
            } else {
                transport          = Transport::Https;
                port               = quint16(server.port(443));
                const QString path = server.path(QUrl::FullyEncoded);
                requestTarget      = (path.isEmpty() ? QStringLiteral("/") : path).toLatin1();
                if (server.hasQuery())
                    requestTarget += '?' + server.query(QUrl::FullyEncoded).toLatin1();
                hostHeader = (host.contains(QLatin1Char(':')) ? '[' + host + ']' : host).toLatin1();
                if (port != 443)
                    hostHeader += ':' + QByteArray::number(port);
            }
            startJdns();
        }
        idleTimer.setSingleShot(true);
        idleTimer.setInterval(IdleTimeout);
        connect(&idleTimer, &QTimer::timeout, this, &Private::closeChannel);
    }

    ~Private() override
    {
        // JDNS cancels its remaining requests on the way out; there is nobody to tell.
        if (dnsTransport)
            dnsTransport->disconnect(this);
        delete dns;
        closeChannel();
    }

    int start(const QByteArray &name, int type, bool longLived)
    {
        const int id = nextId++;
        if (longLived) {
            failLater(id, NameResolver::ErrorNoLongLived);
            return id;
        }

        QByteArray asked = name;
        if (asked.endsWith('.'))
            asked.chop(1);

        // Literal addresses need no DNS, as with the system resolver provider.
        const QHostAddress literal(QString::fromLatin1(asked));
        if (!literal.isNull() && (type == QJDns::A || type == QJDns::Aaaa)) {
            if (type != (literal.protocol() == QAbstractSocket::IPv6Protocol ? QJDns::Aaaa : QJDns::A)) {
                failLater(id, NameResolver::ErrorNoName);
                return id;
            }
            NameRecord record(asked, 0);
            record.setAddress(literal);
            settleLater(id, { record });
            return id;
        }

        if (!dns) {
            errorString = transport == Transport::Invalid ? RemoteNameProvider::tr("Invalid DNS server address")
                                                          : RemoteNameProvider::tr("The DNS resolver failed to start");
            failLater(id, NameResolver::ErrorGeneric);
            return id;
        }
        if (!unreachable.hasExpired()) {
            // A resolution runs several lookups in a row. Do not reconnect (and
            // resend proxy credentials) for each of them when the last attempt
            // just failed; errorString() still tells why.
            failLater(id, NameResolver::ErrorGeneric);
            return id;
        }

        const int query = dns->queryStart(name, type);
        if (query < 0) {
            // Native JDNS can refuse an invalid query synchronously. There
            // will be no resultsReady/error signal for a negative query ID.
            errorString = RemoteNameProvider::tr("Could not start the DNS lookup");
            failLater(id, NameResolver::ErrorGeneric);
            return id;
        }
        lookups.insert(id, { query, asked, type });
        lookupIds.insert(query, id);
        return id;
    }

    void stop(int id)
    {
        settled.remove(id);
        const auto lookup = lookups.find(id);
        if (lookup == lookups.end())
            return;
        lookupIds.remove(lookup->query);
        dns->queryCancel(lookup->query);
        lookups.erase(lookup);
    }

private:
    void startJdns()
    {
        dnsTransport = new QJDnsExternalTransport;
        dnsTransport->setTimeout(RequestTimeout);
        connect(dnsTransport, &QJDnsExternalTransport::queryRequested, this, &Private::onQueryRequested);
        connect(dnsTransport, &QJDnsExternalTransport::requestCancelled, this, &Private::onRequestCancelled);
        dns = new QJDns(this);
        if (!dns->setQueryTransport(dnsTransport)) {
            delete dnsTransport;
            dnsTransport = nullptr;
            delete dns;
            dns = nullptr;
            return;
        }
        if (!dns->init(QJDns::Unicast)) {
            delete dns; // and the transport
            dns          = nullptr;
            dnsTransport = nullptr;
            return;
        }
        connect(dns, &QJDns::resultsReady, this, &Private::onResultsReady);
        connect(dns, &QJDns::error, this, &Private::onQueryError);
        // JDNS keeps its debug output, packet dumps included, until it is fetched.
        connect(dns, &QJDns::debugLinesReady, this, [this]() { dns->debugLines(); });
    }

    /*
       Results are always delivered from the event loop. NameManager reacts by
       starting the next lookups (SRV targets, address fallback), and the
       application may delete this provider; neither belongs inside a JDNS signal.
    */
    void settleLater(int id, const QList<NameRecord> &records)
    {
        settled.insert(id);
        QMetaObject::invokeMethod(
            q,
            [q = q, id, records]() {
                if (q->d->settled.remove(id))
                    emit q->resolve_resultsReady(id, records);
            },
            Qt::QueuedConnection);
    }

    void failLater(int id, NameResolver::Error error)
    {
        settled.insert(id);
        QMetaObject::invokeMethod(
            q,
            [q = q, id, error]() {
                if (q->d->settled.remove(id))
                    emit q->resolve_error(id, error);
            },
            Qt::QueuedConnection);
    }

    // Returns the provider id of a finished JDNS query, or -1 if it was stopped.
    int finish(int query, Lookup *lookup)
    {
        const auto it = lookupIds.find(query);
        if (it == lookupIds.end())
            return -1;
        const int id = *it;
        lookupIds.erase(it);
        *lookup = lookups.take(id);
        return id;
    }

    void onResultsReady(int query, const QJDns::Response &response)
    {
        Lookup    lookup;
        const int id = finish(query, &lookup);
        if (id < 0)
            return;
        QList<NameRecord> records;
        for (QJDns::Record record : response.answerRecords) {
            // only the asked type, so a CNAME chain resolves to the records of its target
            if (lookup.type != QJDns::Any && record.type != lookup.type)
                continue;
            // Like the system resolver provider: records carry the name that
            // was asked for, and names come without the root label.
            record.owner = lookup.name;
            if (record.name.endsWith('.'))
                record.name.chop(1);
            if (record.type == QJDns::Srv && record.name.isEmpty())
                continue; // "." - the service is decidedly not available here (RFC 2782)
            const NameRecord imported = importJDNSRecord(record);
            if (!imported.isNull())
                records += imported;
        }
        if (records.isEmpty())
            failLater(id, NameResolver::ErrorGeneric);
        else
            settleLater(id, records);
    }

    void onQueryError(int query, QJDns::Error error)
    {
        Lookup    lookup;
        const int id = finish(query, &lookup);
        if (id < 0)
            return;
        if (error == QJDns::ErrorNXDomain)
            failLater(id, NameResolver::ErrorNoName);
        else if (error == QJDns::ErrorTimeout) {
            // Only a real timeout marks the remote resolver temporarily
            // unreachable. requestCancelled() is also emitted for ordinary
            // consumer cancellation and must not poison later lookups.
            errorString = RemoteNameProvider::tr("DNS server %1 did not answer in time").arg(host);
            unreachable.setRemainingTime(IdleTimeout);
            failLater(id, NameResolver::ErrorTimeout);
        } else
            failLater(id, NameResolver::ErrorGeneric);
    }

    int indexOfRequest(int id) const
    {
        for (int i = 0; i < requests.size(); ++i) {
            if (requests[i].id == id)
                return i;
        }
        return -1;
    }

    int indexOfWireId(quint16 wireId) const
    {
        for (int i = 0; i < requests.size(); ++i) {
            if (requests[i].sent && requests[i].wireId == wireId)
                return i;
        }
        return -1;
    }

    void onQueryRequested(int id, const QByteArray &message)
    {
        if (!unreachable.hasExpired()) {
            dnsTransport->failRequest(id, errorString);
            return;
        }
        requests.append({ id, message });
        pump();
    }

    // Called both on timeout and when the final consumer cancels. Do not
    // interpret normal cancellation as upstream DNS failure.
    void onRequestCancelled(int id)
    {
        const int index = indexOfRequest(id);
        if (index < 0)
            return;
        const bool lostHttpRequest = requests.takeAt(index).id == httpRequest;
        if (lostHttpRequest) {
            // HTTP replies are sequential: discard the in-flight response
            // rather than accidentally treating it as a later query.
            closeChannel();
        }
        pump();
    }

    void complete(int index, const QByteArray &message)
    {
        const Request request = requests.takeAt(index);
        dnsTransport->completeRequest(request.id, withMessageId(message, readUInt16(request.message, 0)));
    }

    void fail(int index) { dnsTransport->failRequest(requests.takeAt(index).id, errorString); }

    void failAll()
    {
        while (!requests.isEmpty())
            fail(0);
        idle();
    }

    void idle()
    {
        if (!requests.isEmpty())
            return;
        if (channel == Channel::Ready)
            idleTimer.start();
        else
            closeChannel();
    }

    void pump()
    {
        if (requests.isEmpty()) {
            idle();
            return;
        }
        idleTimer.stop();
        if (channel == Channel::Closed) {
            openChannel();
            return;
        }
        if (channel != Channel::Ready)
            return;
        if (transport == Transport::Tcp) {
            // RFC 7766 6.2.1.1: queries may be pipelined; replies are matched by ID
            for (Request &request : requests) {
                if (!request.sent)
                    sendTcp(request);
            }
        } else if (httpRequest < 0) {
            for (Request &request : requests) {
                if (!request.sent) {
                    sendHttp(request);
                    break;
                }
            }
        }
    }

    void openChannel()
    {
        switch (proxy.type()) {
        case QNetworkProxy::Socks5Proxy: {
            auto socks = new SocksClient(this);
            if (!proxy.user().isEmpty())
                socks->setAuth(proxy.user(), proxy.password());
            connect(socks, &SocksClient::connected, this, &Private::onConnected);
            stream = socks;
            break;
        }
        case QNetworkProxy::HttpProxy: {
            auto http = new HttpConnect(this);
            if (!proxy.user().isEmpty())
                http->setAuth(proxy.user(), proxy.password());
            connect(http, &HttpConnect::connected, this, &Private::onConnected);
            stream = http;
            break;
        }
        case QNetworkProxy::NoProxy: {
            auto socket = new BSocket(this);
            connect(socket, &BSocket::connected, this, &Private::onConnected);
            stream = socket;
            break;
        }
        default:
            // Never bypass a proxy the application asked for.
            errorString = RemoteNameProvider::tr("Unsupported proxy type for DNS");
            failAll();
            return;
        }
        connect(stream, &ByteStream::readyRead, this, &Private::onReadyRead);
        connect(stream, &ByteStream::error, this, &Private::onStreamError);
        connect(stream, &ByteStream::connectionClosed, this, &Private::onStreamClosed);
        channel    = Channel::Connecting;
        nextWireId = 0;

        if (auto socks = qobject_cast<SocksClient *>(stream))
            socks->connectToHost(proxy.hostName(), proxy.port(), host, port);
        else if (auto http = qobject_cast<HttpConnect *>(stream))
            http->connectToHost(proxy.hostName(), proxy.port(), host, port);
        else
            static_cast<BSocket *>(stream)->connectToHost(host, port);
    }

    void closeChannel()
    {
        idleTimer.stop();
        channel     = Channel::Closed;
        httpRequest = -1;
        reply.reset();
        tcpBuffer.clear();
        if (tls) {
            tls->disconnect(this);
            tls->deleteLater();
            tls = nullptr;
        }
        if (stream) {
            stream->disconnect(this);
            stream->close();
            stream->deleteLater();
            stream = nullptr;
        }
    }

    // The connection broke while requests were in flight: send them again on a
    // new connection once (servers may close idle connections, RFC 7766 6.2.3).
    void retryOrFail()
    {
        for (int i = requests.size() - 1; i >= 0; --i) {
            Request &request = requests[i];
            if (!request.sent)
                continue;
            if (request.retried) {
                errorString = RemoteNameProvider::tr("The connection to DNS server %1 was lost").arg(host);
                fail(i);
            } else {
                request.retried = true;
                request.sent    = false;
            }
        }
        pump();
    }

    void sendTcp(Request &request)
    {
        // IDs are unique on a connection, so a late reply to a request that
        // timed out cannot be taken for the reply to another one.
        do {
            request.wireId = nextWireId++;
        } while (indexOfWireId(request.wireId) >= 0);
        const QByteArray message = withMessageId(request.message, request.wireId);
        QByteArray       frame;
        appendUInt16(frame, quint16(message.size()));
        frame += message;
        stream->write(frame);
        request.sent = true;
    }

    void sendHttp(Request &request)
    {
        // RFC 8484 4.1: DNS over HTTPS uses message ID 0 for cache friendliness.
        const QByteArray message = withMessageId(request.message, 0);
        QByteArray       http    = "POST " + requestTarget + " HTTP/1.1\r\n";
        http += "Host: " + hostHeader + "\r\n";
        http += "Accept: application/dns-message\r\n";
        http += "Content-Type: application/dns-message\r\n";
        http += "Content-Length: " + QByteArray::number(message.size()) + "\r\n\r\n";
        http += message;
        request.sent = true;
        httpRequest  = request.id;
        reply.emplace();
        tls->write(http);
    }

    void onConnected()
    {
        if (transport == Transport::Https) {
            startTls();
            return;
        }
        channel         = Channel::Ready;
        connectionError = ByteStream::ErrOk;
        pump();
    }

    void startTls()
    {
        if (!QCA::isSupported("tls")) {
            errorString = RemoteNameProvider::tr("TLS is not available for DNS over HTTPS");
            closeChannel();
            failAll();
            return;
        }
        channel = Channel::Handshaking;
        tls     = new QCA::TLS(this);
        tls->setTrustedCertificates(QCA::systemStore());
        connect(tls, &QCA::TLS::handshaken, this, &Private::onTlsHandshaken);
        connect(tls, &QCA::TLS::readyRead, this, [this]() { onHttpData(tls->read()); });
        connect(tls, &QCA::TLS::readyReadOutgoing, this, [this]() { stream->write(tls->readOutgoing()); });
        connect(tls, &QCA::TLS::error, this, &Private::onTlsError);
        tls->startClient(host);
    }

    void onTlsHandshaken()
    {
        if (tls->peerIdentityResult() != QCA::TLS::Valid) {
            errorString = RemoteNameProvider::tr("The certificate of DNS server %1 is not valid").arg(host);
            unreachable.setRemainingTime(IdleTimeout);
            closeChannel();
            failAll();
            return;
        }
        tls->continueAfterStep();
        channel         = Channel::Ready;
        connectionError = ByteStream::ErrOk;
        pump();
    }

    void onTlsError()
    {
        errorString        = RemoteNameProvider::tr("TLS with DNS server %1 failed").arg(host);
        const bool wasUsed = channel == Channel::Ready;
        closeChannel();
        if (wasUsed) {
            retryOrFail();
        } else {
            unreachable.setRemainingTime(IdleTimeout);
            failAll();
        }
    }

    void onReadyRead()
    {
        const QByteArray data = stream->readAll();
        if (tls)
            tls->writeIncoming(data);
        else if (channel == Channel::Ready)
            onTcpData(data);
    }

    void onTcpData(const QByteArray &data)
    {
        tcpBuffer += data;
        while (tcpBuffer.size() >= 2) {
            const int size = readUInt16(tcpBuffer, 0);
            if (tcpBuffer.size() < 2 + size)
                break;
            const QByteArray message = tcpBuffer.mid(2, size);
            tcpBuffer.remove(0, 2 + size);
            const int index = message.size() >= 2 ? indexOfWireId(readUInt16(message, 0)) : -1;
            if (index >= 0)
                complete(index, message); // otherwise the request timed out
        }
        if (requests.isEmpty())
            idle();
    }

    void onHttpData(const QByteArray &data)
    {
        if (reply)
            onHttpReply(reply->feed(data));
    }

    void onHttpReply(HttpReply::Result result)
    {
        if (result == HttpReply::NeedMore)
            return;
        const HttpReply finished = *reply;
        const int       index    = indexOfRequest(httpRequest);
        reply.reset();
        httpRequest = -1;

        if (result == HttpReply::Invalid) {
            errorString = RemoteNameProvider::tr("Invalid HTTP reply from DNS server %1").arg(host);
            if (index >= 0)
                fail(index);
            closeChannel();
        } else {
            if (index >= 0) {
                if (finished.status == 200) {
                    complete(index, finished.body);
                } else {
                    errorString = RemoteNameProvider::tr("DNS server %1 answered with HTTP status %2")
                                      .arg(host)
                                      .arg(finished.status);
                    fail(index);
                }
            }
            if (!finished.keepAlive)
                closeChannel();
        }
        pump();
    }

    void onStreamError(int code)
    {
        const bool wasUsed = channel == Channel::Ready;
        if (!wasUsed) {
            connectionError = code;
            errorString     = streamErrorString(code);
            unreachable.setRemainingTime(IdleTimeout);
        }
        closeChannel();
        if (wasUsed)
            retryOrFail();
        else
            failAll();
    }

    void onStreamClosed()
    {
        if (reply) {
            const auto result = reply->finish();
            if (result == HttpReply::Done) {
                onHttpReply(result); // closes the channel: the reply was not keep-alive
                return;
            }
        }
        const bool wasUsed = channel == Channel::Ready;
        closeChannel();
        if (wasUsed) {
            retryOrFail();
        } else {
            errorString = streamErrorString(ByteStream::ErrRead);
            unreachable.setRemainingTime(IdleTimeout);
            failAll();
        }
    }
};
#else
// Without JDNS nothing is resolved, and no lookup falls back to local DNS.
class RemoteNameProvider::Private {
public:
    RemoteNameProvider *q;
    int                 nextId = 0;
    QSet<int>           settled;
    int                 connectionError = ByteStream::ErrOk;
    QString             errorString;

    Private(RemoteNameProvider *q, const QUrl &, const QNetworkProxy &) : q(q) { }

    int start(const QByteArray &, int, bool)
    {
        const int id = nextId++;
        errorString  = RemoteNameProvider::tr("Remote DNS is not available: Iris was built without JDNS");
        settled.insert(id);
        QMetaObject::invokeMethod(
            q,
            [q = q, id]() {
                if (q->d->settled.remove(id))
                    emit q->resolve_error(id, NameResolver::ErrorGeneric);
            },
            Qt::QueuedConnection);
        return id;
    }

    void stop(int id) { settled.remove(id); }
};
#endif

RemoteNameProvider::RemoteNameProvider(const QUrl &server, const QNetworkProxy &proxy, QObject *parent) :
    NameProvider(parent), d(std::make_unique<Private>(this, server, proxy))
{
}

RemoteNameProvider::~RemoteNameProvider() = default;

bool RemoteNameProvider::isSupported()
{
#ifdef IRISNET_HAVE_JDNS
    return true;
#else
    return false;
#endif
}

bool RemoteNameProvider::isValidServer(const QUrl &server)
{
    if (!server.isValid() || server.host().isEmpty() || !server.userInfo().isEmpty() || server.hasFragment())
        return false;

    const int port = server.port(-1);
    if (port == 0 || port > 65535)
        return false;

    if (server.scheme() == QLatin1String("https"))
        return true;

    // DNS-over-TCP has no HTTP-style path or query: accepting one would
    // silently discard configuration the caller might expect to be used.
    return server.scheme() == QLatin1String("tcp") && (server.path().isEmpty() || server.path() == QLatin1String("/"))
        && !server.hasQuery();
}

int RemoteNameProvider::connectionError() const { return d->connectionError; }

QString RemoteNameProvider::errorString() const { return d->errorString; }

bool RemoteNameProvider::supportsSingle() const { return true; }

bool RemoteNameProvider::supportsLongLived() const { return false; }

bool RemoteNameProvider::supportsRecordType(int type) const
{
#ifdef IRISNET_HAVE_JDNS
    // the types NameRecord holds, see importJDNSRecord()
    switch (type) {
    case QJDns::A:
    case QJDns::Aaaa:
    case QJDns::Mx:
    case QJDns::Srv:
    case QJDns::Cname:
    case QJDns::Ptr:
    case QJDns::Txt:
    case QJDns::Hinfo:
    case QJDns::Ns:
    case TypeNull:
    case QJDns::Any:
        return true;
    default:
        return false;
    }
#else
    Q_UNUSED(type)
    return false;
#endif
}

int RemoteNameProvider::resolve_start(const QByteArray &name, int qType, bool longLived)
{
    return d->start(name, qType, longLived);
}

void RemoteNameProvider::resolve_stop(int id) { d->stop(id); }
} // namespace XMPP
