/*
 * httpfileupload.cpp - HTTP File upload
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

#include <QList>

#include "httpfileupload.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"

using namespace XMPP;

static QLatin1String xmlns_v0_2_5("urn:xmpp:http:upload");
static QLatin1String xmlns_v0_3_1("urn:xmpp:http:upload:0");

//----------------------------------------------------------------------------
// HttpFileUpload
//----------------------------------------------------------------------------
class HttpFileUpload::Private
{
public:
    HttpFileUpload::State state;
    XMPP::Client *client;
    JT_DiscoItems *jtDiscoItems;
    QList<JT_DiscoInfo *> jtDiscoInfo;
    JT_HTTPFileUpload *jtHttpSlot;
    quint64 fileSize;
    QString fileName;
    QString mediaType;
    QList<HttpHost> httpHosts;
    int hostIndex;

    struct {
        int statusCode;
        QString statusString;
        QString get_url;
        QString put_url;
        XEP0363::HttpHeaders headers;
        quint64 sizeLimit;
    } result;

    void httpHostsUpdate(const HttpHost &host)
    {
        int cnt = httpHosts.size();
        for (int i = 0; i < cnt; ++i) {
            if (httpHosts.at(i).jid == host.jid) {
                httpHosts[i] = host;
                return;
            }
        }
        httpHosts.append(host);
    }
};

HttpFileUpload::HttpFileUpload(XMPP::Client *client, QObject *parent)
    : QObject(parent)
{
    d = new Private;
    d->client = client;
    init();
}

HttpFileUpload::~HttpFileUpload()
{
    qDeleteAll(d->jtDiscoInfo);
    delete d->jtDiscoItems;
    delete d->jtHttpSlot;
    delete d;
}

void HttpFileUpload::init()
{
    d->state = State::None;
    d->jtDiscoItems = nullptr;
    d->jtHttpSlot = nullptr;
    d->httpHosts.clear();
    d->hostIndex = -1;
    d->result.statusCode = 0;
    d->result.statusString = "";
    d->result.get_url = "";
    d->result.put_url = "";
    d->result.headers.clear();
    d->result.sizeLimit = 0;
}

void HttpFileUpload::start(const QString &fname, quint64 fsize, const QString &mType)
{
    if (d->state != State::None) // Attempt to start twice?
        return;

    d->state = State::GettingItems;
    d->fileName = fname;
    d->fileSize = fsize;
    d->mediaType = mType;
    d->hostIndex = -1;
    d->result.statusCode = 0;
    if (d->httpHosts.isEmpty()) {
        d->jtDiscoItems = new JT_DiscoItems(d->client->rootTask());
        connect(d->jtDiscoItems, SIGNAL(finished()), this, SLOT(discoItemsFinished()), Qt::QueuedConnection);
        d->jtDiscoItems->get(d->client->jid().domain());
        d->jtDiscoItems->go(true);
    }
    else
        sendHttpSlotRequest();
}

bool HttpFileUpload::success() const
{
    return d->state == State::Success;
}

int HttpFileUpload::statusCode() const
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
        slot.get.url = d->result.get_url;
        slot.put.url = d->result.put_url;
        slot.put.headers = d->result.headers;
        slot.limits.fileSize = d->result.sizeLimit;
    }
    return slot;
}

void HttpFileUpload::discoItemsFinished()
{
    JT_DiscoItems *jt = d->jtDiscoItems;
    d->jtDiscoItems = nullptr;
    if (jt->success()) {
        d->state = State::SendingInfoQueryes;
        foreach(const DiscoItem &item, jt->items()) {
            sendDiscoInfoRequest(item);
        }
        d->state = State::WaitingDiscoInfo;
    }
    else {
        d->result.statusCode   = jt->statusCode();
        d->result.statusString = jt->statusString();
        done(State::Error);
    }
}

void HttpFileUpload::discoInfoFinished()
{
    JT_DiscoInfo *jt = static_cast<JT_DiscoInfo *>(sender());
    d->jtDiscoInfo.removeOne(jt);
    if (!jt->success()) {
        // It can be better to continue the search
        d->result.statusCode   = jt->statusCode();
        d->result.statusString = jt->statusString();
        //done(State::Error);
        return;
    }

    const QStringList &l = jt->item().features().list();
    XEP0363::version ver = XEP0363::vUnknown;
    quint64 sizeLimit = 0;
    if (l.contains(xmlns_v0_3_1))
        ver = XEP0363::v0_3_1;
    else if (l.contains(xmlns_v0_2_5))
        ver = XEP0363::v0_2_5;
    if (ver != XEP0363::vUnknown) {
        const XData::Field field = jt->item().registeredExtension("jabber:x:data").getField("max-file-size");
        if (field.isValid() && field.type() == XData::Field::Field_TextSingle)
            sizeLimit = field.value().at(0).toULongLong();
        HttpHost host;
        host.ver = ver;
        host.jid = jt->item().jid();
        host.sizeLimit = sizeLimit;
        host.props = SecureGet | SecurePut;
        if (ver == XEP0363::v0_3_1)
            host.props |= NewestVer;
        d->httpHostsUpdate(host);
    }

    if (d->state == State::WaitingDiscoInfo) {
        if (d->httpHosts.size() > 0) {
            if (d->hostIndex == -1)
                sendHttpSlotRequest();
        }
        else if (d->jtDiscoInfo.size() == 0) {
            if (d->result.statusCode == 0) {
                d->result.statusCode   = -1;
                d->result.statusString = "Http upload items have not been found";
            }
            done(State::Error);
        }
    }
}

void HttpFileUpload::httpSlotFinished()
{
    JT_HTTPFileUpload *jt = d->jtHttpSlot;
    d->jtHttpSlot = nullptr;
    HttpHost h = d->httpHosts[d->hostIndex];
    if (jt->success()) {
        d->result.get_url = jt->url(JT_HTTPFileUpload::GetUrl);
        d->result.put_url = jt->url(JT_HTTPFileUpload::PutUrl);
        d->result.headers = jt->headers();
        if (d->result.get_url.startsWith("https://"))
            h.props |= SecureGet;
        else
            h.props &= ~SecureGet;
        if (d->result.put_url.startsWith("https://"))
            h.props |= SecurePut;
        else
            h.props &= ~SecurePut;
        h.props &= ~Failure;
        done(State::Success);
    }
    else {
        h.props |= Failure;
        d->result.statusCode   = jt->statusCode();
        d->result.statusString = jt->statusString();
        done(State::Error);
    }
    d->httpHostsUpdate(h);
}

void HttpFileUpload::sendDiscoInfoRequest(const DiscoItem &item)
{
    JT_DiscoInfo *jt = new JT_DiscoInfo(d->client->rootTask());
    connect(jt, SIGNAL(finished()), this, SLOT(discoInfoFinished()), Qt::QueuedConnection);
    d->jtDiscoInfo.append(jt);
    jt->get(item);
    jt->go(true);
}

void HttpFileUpload::sendHttpSlotRequest()
{
    d->hostIndex = selectHost();
    if (d->hostIndex != -1) {
        HttpHost host = d->httpHosts.at(d->hostIndex);
        d->result.sizeLimit = host.sizeLimit;
        d->jtHttpSlot = new JT_HTTPFileUpload(d->client->rootTask());
        connect(d->jtHttpSlot, SIGNAL(finished()), this, SLOT(httpSlotFinished()), Qt::QueuedConnection);
        d->jtHttpSlot->request(host.jid, d->fileName, d->fileSize, d->mediaType, host.ver);
        d->jtHttpSlot->go(true);
    }
}

void HttpFileUpload::done(State state)
{
    d->state = state;
    emit finished();
}

int HttpFileUpload::selectHost() const
{
    int selIdx = -1;
    int selVal = 0;
    for (int i = 0; i < d->httpHosts.size(); ++i) {
        if (d->fileSize >= d->httpHosts[i].sizeLimit) {
            int props = d->httpHosts[i].props;
            int val = 0;
            if (props & SecureGet) val += 5;
            if (props & SecurePut) val += 5;
            if (props & NewestVer) val += 3;
            if (props & Failure) val -= 15;
            if (selIdx == -1 || val > selVal) {
                selIdx = i;
                selVal = val;
            }
        }
    }
    return selIdx;
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
        setError(900);
        return true;
    }
    if (!getUrl.isEmpty() && !putUrl.isEmpty()) {
        d->urls[GetUrl] = getUrl;
        d->urls[PutUrl] = putUrl;
        d->headers      = headers;
        setSuccess();
    }
    else
        setError(900, "Either `put` or `get` URL is missing in the server's reply.");
    return true;
}
