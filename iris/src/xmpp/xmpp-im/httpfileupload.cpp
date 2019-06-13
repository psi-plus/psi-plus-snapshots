/*
 * httpfileupload.cpp - HTTP File upload
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

#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegExp>

#include "httpfileupload.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"
#include "xmpp_serverinfomanager.h"

using namespace XMPP;

static QLatin1String xmlns_v0_2_5("urn:xmpp:http:upload");
static QLatin1String xmlns_v0_3_1("urn:xmpp:http:upload:0");

//----------------------------------------------------------------------------
// HttpFileUpload
//----------------------------------------------------------------------------
class HttpFileUpload::Private
{
public:
    HttpFileUpload::State state = State::None;
    XMPP::Client *client = nullptr;
    QIODevice *sourceDevice = nullptr;
    QPointer<QNetworkAccessManager> qnam = nullptr;
    quint64 fileSize = 0;
    QString fileName;
    QString mediaType;
    QList<HttpHost> httpHosts;

    struct {
        HttpFileUpload::ErrorCode statusCode = HttpFileUpload::ErrorCode::NoError;
        QString statusString;
        QString getUrl;
        QString putUrl;
        XEP0363::HttpHeaders putHeaders;
        quint64 sizeLimit = 0;
    } result;
};

HttpFileUpload::HttpFileUpload(XMPP::Client *client, QIODevice *source, size_t fsize, const QString &dstFilename,
                               const QString &mType) :
    QObject(client),
    d(new Private)
{
    d->client = client;
    d->sourceDevice = source;
    d->fileName = dstFilename;
    d->fileSize = fsize;
    d->mediaType = mType;
}

HttpFileUpload::~HttpFileUpload()
{
    qDebug("destroying");
}

void HttpFileUpload::setNetworkAccessManager(QNetworkAccessManager *qnam)
{
    d->qnam = qnam;
}

void HttpFileUpload::start()
{
    if (d->state != State::None) // Attempt to start twice?
        return;

    setState(State::GettingSlot);

    d->result.statusCode = HttpFileUpload::ErrorCode::NoError;
    static QList<QSet<QString>> featureOptions;
    if (featureOptions.isEmpty()) {
        featureOptions << (QSet<QString>() << xmlns_v0_2_5) << (QSet<QString>() << xmlns_v0_3_1);
    }
    d->client->serverInfoManager()->queryServiceInfo(
                QLatin1String("store"), QLatin1String("file"),
                featureOptions, QRegExp("^(upload|http|stor|file|dis|drive).*"), ServerInfoManager::SQ_CheckAllOnNoMatch,
                [this](const QList<DiscoItem> &items)
    {
        d->httpHosts.clear();
        for (const auto &item: items) {
            const QStringList &l = item.features().list();
            XEP0363::version ver = XEP0363::vUnknown;
            QString xmlns;
            quint64 sizeLimit = 0;
            if (l.contains(xmlns_v0_3_1)) {
                ver = XEP0363::v0_3_1;
                xmlns = xmlns_v0_3_1;
            } else if (l.contains(xmlns_v0_2_5)) {
                ver = XEP0363::v0_2_5;
                xmlns = xmlns_v0_2_5;
            }
            if (ver != XEP0363::vUnknown) {
                QList<std::pair<HttpHost,int>> hosts;
                const XData::Field field = item.registeredExtension(xmlns).getField(QLatin1String("max-file-size"));
                if (field.isValid() && field.type() == XData::Field::Field_TextSingle)
                    sizeLimit = field.value().at(0).toULongLong();
                HttpHost host;
                host.ver = ver;
                host.jid = item.jid();
                host.sizeLimit = sizeLimit;
                QVariant metaProps(d->client->serverInfoManager()->serviceMeta(host.jid, "httpprops"));
                if (metaProps.isValid()) {
                    host.props = HostProps(metaProps.value<int>());
                } else {
                    host.props = SecureGet | SecurePut;
                    if (ver == XEP0363::v0_3_1)
                        host.props |= NewestVer;
                }
                int value = 0;
                if (host.props & SecureGet) value += 5;
                if (host.props & SecurePut) value += 5;
                if (host.props & NewestVer) value += 3;
                if (host.props & Failure) value -= 15;
                if (!sizeLimit || d->fileSize < sizeLimit)
                    hosts.append({host,value});

                // no sorting in preference order. most preferred go first
                std::sort(hosts.begin(), hosts.end(), [](const auto &a, const auto &b){
                    return a.second > b.second;
                });
                for (auto &hp: hosts) {
                    d->httpHosts.append(hp.first);
                }
            }
        }
        //d->currentHost = d->httpHosts.begin();
        if (d->httpHosts.isEmpty()) { // if empty as the last resort check all services
            d->result.statusCode   = HttpFileUpload::ErrorCode::NoUploadService;
            d->result.statusString = "No suitable http upload services were found";
            done(State::Error);
        } else {
            tryNextServer();
        }
    });
}

void HttpFileUpload::tryNextServer()
{
    if (d->httpHosts.isEmpty()) { // if empty as the last resort check all services
        d->result.statusCode   = HttpFileUpload::ErrorCode::NoUploadService;
        d->result.statusString = "All http services are either non compliant or returned errors";
        done(State::Error);
        return;
    }
    HttpHost host = d->httpHosts.takeFirst();
    d->result.sizeLimit = host.sizeLimit;
    auto jt = new JT_HTTPFileUpload(d->client->rootTask());
    connect(jt, &JT_HTTPFileUpload::finished, this, [this, jt, host]() mutable {
        if (!jt->success()) {
            host.props |= Failure;
            int code = jt->statusCode();
            if (code < 300) {
                code++; // ErrDisc and ErrTimeout. but 0 code is already occupated
            }
            d->result.statusCode   = static_cast<ErrorCode>(jt->statusCode());
            d->result.statusString = jt->statusString();
            d->client->serverInfoManager()->setServiceMeta(host.jid, QLatin1String("httpprops"), int(host.props));
            if (d->httpHosts.isEmpty())
                done(State::Error);
            else
                tryNextServer();
            return;
        }

        d->result.getUrl = jt->url(JT_HTTPFileUpload::GetUrl);
        d->result.putUrl = jt->url(JT_HTTPFileUpload::PutUrl);
        d->result.putHeaders = jt->headers();
        if (d->result.getUrl.startsWith("https://"))
            host.props |= SecureGet;
        else
            host.props &= ~SecureGet;
        if (d->result.putUrl.startsWith("https://"))
            host.props |= SecurePut;
        else
            host.props &= ~SecurePut;
        host.props &= ~Failure;

        d->client->serverInfoManager()->setServiceMeta(host.jid, QLatin1String("httpprops"), int(host.props));

        if (!d->qnam) { // w/o network access manager, it's not more than getting slots
            done(State::Success);
            return;
        }

        setState(State::HttpRequest);
        // time for a http request
        QNetworkRequest req(d->result.putUrl);
        for (auto &h: d->result.putHeaders) {
            req.setRawHeader(h.name.toLatin1(), h.value.toLatin1());
        }
        auto reply = d->qnam->put(req, d->sourceDevice);
        connect(reply, &QNetworkReply::finished, this, [this, reply](){
            if (reply->error() == QNetworkReply::NoError) {
                done(State::Success);
            } else {
                d->result.statusCode   = ErrorCode::HttpFailed;
                d->result.statusString = reply->errorString();
                if (d->httpHosts.isEmpty())
                    done(State::Error);
                else
                    tryNextServer();
            }
            reply->deleteLater();
        });

    }, Qt::QueuedConnection);
    jt->request(host.jid, d->fileName, d->fileSize, d->mediaType, host.ver);
    jt->go(true);
}

bool HttpFileUpload::success() const
{
    return d->state == State::Success;
}

HttpFileUpload::ErrorCode HttpFileUpload::statusCode() const
{
    return d->result.statusCode;
}

const QString & HttpFileUpload::statusString() const
{
    return d->result.statusString;
}

HttpFileUpload::HttpSlot HttpFileUpload::getHttpSlot()
{
    HttpSlot slot;
    if (d->state == State::Success) {
        slot.get.url = d->result.getUrl;
        slot.put.url = d->result.putUrl;
        slot.put.headers = d->result.putHeaders;
        slot.limits.fileSize = d->result.sizeLimit;
    }
    return slot;
}

void HttpFileUpload::setState(State state)
{
    d->state = state;
    if (state == Success) {
        d->result.statusCode = ErrorCode::NoError;
        d->result.statusString.clear();
    }
    emit stateChanged();
}

void HttpFileUpload::done(State state)
{
    setState(state);
    emit finished();
}

//----------------------------------------------------------------------------
// JT_HTTPFileUpload
//----------------------------------------------------------------------------
class JT_HTTPFileUpload::Private
{
public:
    Jid to;
    QDomElement iq;
    QStringList urls;
    XEP0363::version ver;
    XEP0363::HttpHeaders headers;
};

JT_HTTPFileUpload::JT_HTTPFileUpload(Task *parent)
    : Task(parent)
{
    d = new Private;
    d->ver = XEP0363::vUnknown;
    d->urls << QString() << QString();
}

JT_HTTPFileUpload::~JT_HTTPFileUpload()
{
    delete d;
}

void JT_HTTPFileUpload::request(const Jid &to, const QString &fname,
                                quint64 fsize, const QString &ftype, XEP0363::version ver)
{
    d->to = to;
    d->ver = ver;
    d->iq = createIQ(doc(), "get", to.full(), id());
    QDomElement req = doc()->createElement("request");
    switch (ver)
    {
    case XEP0363::v0_2_5:
        req.setAttribute("xmlns", xmlns_v0_2_5);
        req.appendChild(textTag(doc(), "filename", fname));
        req.appendChild(textTag(doc(), "size", QString::number(fsize)));
        if (!ftype.isEmpty()) {
            req.appendChild(textTag(doc(), "content-type", ftype));
        }
        break;
    case XEP0363::v0_3_1:
        req.setAttribute("xmlns", xmlns_v0_3_1);
        req.setAttribute("filename", fname);
        req.setAttribute("size", fsize);
        if (!ftype.isEmpty())
            req.setAttribute("content-type", ftype);
        break;
    default:
        d->ver = XEP0363::vUnknown;
        break;
    }
    d->iq.appendChild(req);
}

QString JT_HTTPFileUpload::url(UrlType t) const
{
    return d->urls.value(t);
}

XEP0363::HttpHeaders JT_HTTPFileUpload::headers() const
{
    return d->headers;
}

void JT_HTTPFileUpload::onGo()
{
    if (d->ver != XEP0363::vUnknown)
        send(d->iq);
}

bool JT_HTTPFileUpload::take(const QDomElement &e)
{
    if (!iqVerify(e, d->to, id()))
        return false;

    if (e.attribute("type") != "result") {
        setError(e);
        return true;
    }

    bool correct_xmlns = false;
    QString getUrl, putUrl;
    XEP0363::HttpHeaders headers;
    const QDomElement &slot = e.firstChildElement("slot");
    if (!slot.isNull()) {
        const QDomElement &get = slot.firstChildElement("get");
        const QDomElement &put = slot.firstChildElement("put");
        switch (d->ver)
        {
        case XEP0363::v0_2_5:
            correct_xmlns = slot.attribute("xmlns") == xmlns_v0_2_5;
            getUrl = tagContent(get);
            putUrl = tagContent(put);
            break;
        case XEP0363::v0_3_1:
            correct_xmlns = slot.attribute("xmlns") == xmlns_v0_3_1;
            getUrl = get.attribute("url");
            if (!put.isNull()) {
                putUrl = put.attribute("url");
                QDomElement he = put.firstChildElement("header");
                while (!he.isNull()) {
                    // only next are allowed: Authorization, Cookie, Expires
                    QString header = he.attribute("name").trimmed().remove(QLatin1Char('\n'));
                    QString value = he.text().trimmed().remove(QLatin1Char('\n'));
                    if (!value.isEmpty() &&
                            (header.compare(QLatin1String("Authorization"), Qt::CaseInsensitive) == 0 ||
                            header.compare(QLatin1String("Cookie"), Qt::CaseInsensitive) == 0 ||
                            header.compare(QLatin1String("Expires"), Qt::CaseInsensitive) == 0))
                    {
                        headers.append(XEP0363::HttpHeader{header, value});
                    }
                    he = he.nextSiblingElement("header");
                }
            }
            break;
        default:
            break;
        }
    }
    if (!correct_xmlns) {
        setError(ErrInvalidResponse);
        return true;
    }
    if (!getUrl.isEmpty() && !putUrl.isEmpty()) {
        d->urls[GetUrl] = getUrl;
        d->urls[PutUrl] = putUrl;
        d->headers      = headers;
        setSuccess();
    }
    else
        setError(ErrInvalidResponse, "Either `put` or `get` URL is missing in the server's reply.");
    return true;
}


class HttpFileUploadManager::Private {
public:
    Client *client = nullptr;
    QPointer<QNetworkAccessManager> qnam;
    bool externalQnam = false;
    QLinkedList<HttpFileUpload::HttpHost> hosts;
};



HttpFileUploadManager::HttpFileUploadManager(Client *parent) :
    QObject(parent),
    d(new Private)
{
    d->client = parent;
}

HttpFileUploadManager::~HttpFileUploadManager()
{
    delete d;
}

void HttpFileUploadManager::setNetworkAccessManager(QNetworkAccessManager *qnam)
{
    d->externalQnam = true;
    d->qnam = qnam;
}

HttpFileUpload* HttpFileUploadManager::upload(const QString &srcFilename, const QString &dstFilename, const QString &mType)
{
    auto f = new QFile(srcFilename);
    f->open(QIODevice::ReadOnly);
    auto hfu = upload(f, f->size(), dstFilename, mType);
    f->setParent(hfu);
    return hfu;
}

HttpFileUpload* HttpFileUploadManager::upload(QIODevice *source, size_t fsize, const QString &dstFilename, const QString &mType)
{
    auto hfu = new HttpFileUpload(d->client, source, fsize, dstFilename, mType);
    QNetworkAccessManager *qnam = d->externalQnam? d->qnam.data() : d->client->networkAccessManager();
    hfu->setNetworkAccessManager(qnam);
    QMetaObject::invokeMethod(hfu, "start", Qt::QueuedConnection);
    return hfu;
}
