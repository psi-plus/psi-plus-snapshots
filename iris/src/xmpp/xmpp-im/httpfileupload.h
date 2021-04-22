/*
 * httpfileupload.h - HTTP File upload
 * Copyright (C) 2017-2019  Aleksey Andreev, Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_HTTPFILEUPLOAD_H
#define XMPP_HTTPFILEUPLOAD_H

#include "xmpp/jid/jid.h"
#include "xmpp_task.h"

#include <functional>
#include <memory>

class QIODevice;
class QNetworkAccessManager;

namespace XMPP {
class Client;
namespace XEP0363 {
    enum version { vUnknown, v0_2_5, v0_3_1 };
    struct HttpHeader {
        QString name;
        QString value;
    };
    typedef QList<HttpHeader> HttpHeaders;
}

class HttpFileUpload : public QObject {
    Q_OBJECT
public:
    enum HostPropFlag {
        SecureGet = 1, // 0.2.5 of the xep didn't require that
        SecurePut = 2, // 0.2.5 of the xep didn't require that
        NewestVer = 4,
        Failure   = 8 // had some failure (no/unexpected response to slot request, early http errors)
    };
    Q_DECLARE_FLAGS(HostProps, HostPropFlag)

    enum class ErrorCode : int {
        NoError = 0,
        XmppConnectionFailure,
        Timeout,
        SlotReceiveFailed,
        NoUploadService = 5, // previous could be mapped to Task errors
        HttpFailed
    };

    struct HttpSlot {
        struct {
            QString url;
        } get;
        struct {
            QString                    url;
            QList<XEP0363::HttpHeader> headers;
        } put;
        struct {
            quint64 fileSize;
        } limits;
    };

    struct HttpHost {
        XEP0363::version ver;
        Jid              jid;
        quint64          sizeLimit;
        HostProps        props;
    };

    HttpFileUpload(Client *client, QIODevice *source, size_t fsize, const QString &dstFilename,
                   const QString &mType = QString());
    HttpFileUpload(const HttpFileUpload &) = delete;
    ~HttpFileUpload();

    /**
     * @brief setNetworkAccessManager sets network access manager to do http requests.
     * @param qnam network access manager instance
     *
     * HttpFileUpload by default stops after receiving an http slot from the xmpp server.
     * setting qnam allows doing automatic http requests after getting slot,
     * so finished signal will be emitted when http finished.
     */
    void setNetworkAccessManager(QNetworkAccessManager *qnam);

    bool           success() const;
    ErrorCode      statusCode() const;
    const QString &statusString() const;
    HttpSlot       getHttpSlot();

public slots:
    void start();

signals:
    void stateChanged();
    void finished();
    void progress(qint64 bytesReceived, qint64 bytesTotal);

private:
    enum State { None, GettingSlot, HttpRequest, Success, Error };
    friend class HttpFileUploadManager;

    void init();
    void done(State state);
    void tryNextServer();
    void setState(State state);

private:
    class Private;
    std::unique_ptr<Private> d;
};
Q_DECLARE_OPERATORS_FOR_FLAGS(HttpFileUpload::HostProps)

class JT_HTTPFileUpload : public Task {
    Q_OBJECT
public:
    enum UrlType { GetUrl = 0, PutUrl = 1 };
    enum {
        ErrInvalidResponse = int(HttpFileUpload::ErrorCode::SlotReceiveFailed) - 1
    }; // -1 to be mapped to ErrDisc, ErrTimeout, ...

    JT_HTTPFileUpload(Task *parent);
    ~JT_HTTPFileUpload();

    void    request(const Jid &to, const QString &fname, quint64 fsize, const QString &ftype, XEP0363::version ver);
    QString url(UrlType t) const;
    XEP0363::HttpHeaders headers() const;

    void onGo();
    bool take(const QDomElement &);

private:
    class Private;
    Private *d;
};

class HttpFileUploadManager : public QObject {
    Q_OBJECT
public:
    enum { DiscoNone = 0x0, DiscoNotFound = 0x1, DiscoFound = 0x2 };

    typedef std::function<void(bool, const QString &)>
        Callback; // params: success, detail. where detail could be a "get" url

    HttpFileUploadManager(Client *parent);
    ~HttpFileUploadManager();

    int discoveryStatus() const;

    /**
     * @brief setNetworkAccessManager sets network access manager to do http requests.
     * @param qnam network access manager instance
     *
     * HttpFileUpload by default stops after receiving an http slot from the xmpp server.
     * setting qnam allows doing automatic http requests after getting slot,
     * so finished signal will be emitted when http finished.
     *
     * NOTE: by default QNAM from Client will be in use until something set with this method.
     *       So it's possible to disable HTTP part by setting NULL here.
     */
    void setNetworkAccessManager(QNetworkAccessManager *qnam);

    /**
     * @brief uploads given file to http server
     * @param srcFilename name of the real file on the filesystem
     * @param dstFilename name of remote/target file
     * @param mType meta type. image/png for example
     * @return returns a handler object which will signal "finished" when ready
     */
    HttpFileUpload *upload(const QString &srcFilename, const QString &dstFilename = QString(),
                           const QString &mType = QString());

    /**
     * @brief uploads data of given size from the given to remote server
     * @param source - source device
     * @param fsize - size of data to upload
     * @param dstFilename - name of file on the remote server
     * @param mType - meta type
     * @return returns a handler object which will signal "finished" when ready
     */
    HttpFileUpload *upload(QIODevice *source, quint64 fsize, const QString &dstFilename,
                           const QString &mType = QString());

private:
    friend class HttpFileUpload;
    const QList<HttpFileUpload::HttpHost> &discoHosts() const;
    void                                   setDiscoHosts(const QList<HttpFileUpload::HttpHost> &hosts);

    class Private;
    Private *d;
};
} // namespace XMPP

#endif // XMPP_HTTPFILEUPLOAD_H
