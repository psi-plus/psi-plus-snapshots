/*
 * httpfileupload.h - HTTP File upload
 * Copyright (C) 2017  Aleksey Andreev
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef XMPP_HTTPFILEUPLOAD_H
#define XMPP_HTTPFILEUPLOAD_H

#include <memory>

#include "im.h"

namespace XMPP
{
    namespace XEP0363 {
        enum version { vUnknown, v0_2_5, v0_3_1 };
        struct HttpHeader { QString name; QString value; };
        typedef QList<HttpHeader> HttpHeaders;
    }

    class HttpFileUpload : public QObject
    {
        Q_OBJECT
    public:
        enum HostPropFlag { SecureGet = 1, SecurePut = 2, NewestVer = 4, Failure = 8 };
        Q_DECLARE_FLAGS(HostProps, HostPropFlag)
        struct HttpSlot {
            struct {
                QString url;
            } get;
            struct {
                QString url;
                QList<XEP0363::HttpHeader> headers;
            } put;
            struct {
                quint64 fileSize;
            } limits;
        };

        HttpFileUpload(Client *client, QObject *parent = nullptr);
        HttpFileUpload(const HttpFileUpload &) = delete;
        ~HttpFileUpload();

        void start(const QString &fname, quint64 fsize, const QString &mType = QString::null);
        bool success() const;
        int  statusCode() const;
        const QString & statusString() const;
        HttpSlot getHttpSlot();

    signals:
        void finished();

    private slots:
        void discoInfoFinished();
        void httpSlotFinished();

    private:
        enum State { None, GettingItems, SendingInfoQueryes, WaitingDiscoInfo, Success, Error };
        struct HttpHost {
            XEP0363::version ver;
            Jid jid;
            quint64 sizeLimit;
            HostProps props;
        };
        void init();
        void sendDiscoInfoRequest(const DiscoItem &item);
        void sendHttpSlotRequest();
        void done(State state);
        int selectHost() const;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };
    Q_DECLARE_OPERATORS_FOR_FLAGS(HttpFileUpload::HostProps)

    class JT_HTTPFileUpload : public Task
    {
        Q_OBJECT
    public:
        enum UrlType { GetUrl = 0, PutUrl = 1 };

        JT_HTTPFileUpload(Task *parent);
        ~JT_HTTPFileUpload();

        void request(const Jid &to, const QString &fname,
                     quint64 fsize, const QString &ftype, XEP0363::version ver);
        QString url(UrlType t) const;
        XEP0363::HttpHeaders headers() const;

        void onGo();
        bool take(const QDomElement &);

    private:
        class Private;
        Private *d;
    };
}

#endif
