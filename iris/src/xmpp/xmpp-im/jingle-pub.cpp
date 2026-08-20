/*
 * jingle-pub.cpp - XEP-0358 Publishing Available Jingle Sessions
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "jingle-pub.h"

#include "jingle.h"
#include "xmpp_client.h"
#include "xmpp_task.h"
#include "xmpp_xmlcommon.h"

#include <QDomDocument>
#include <QPointer>
#include <QSet>

#include <iterator>

namespace XMPP::Jingle {

const QString JINGLEPUB_NS = QStringLiteral("urn:xmpp:jinglepub:1");

class JinglePub::Private : public QSharedData {
public:
    Jid                from;
    QString            id;
    QUrl               uri;
    QList<Meta>        meta;
    QList<QDomElement> descriptions;
};

JinglePub::JinglePub() { }
JinglePub::JinglePub(const QDomElement &element) { fromXml(element); }
JinglePub::JinglePub(const JinglePub &)            = default;
JinglePub &JinglePub::operator=(const JinglePub &) = default;
JinglePub::~JinglePub()                            = default;

JinglePub::Private *JinglePub::ensureD()
{
    if (!d)
        d = new Private;
    return d.data();
}

bool JinglePub::isValid() const
{
    if (!d || !d->from.isValid() || d->id.isEmpty() || d->descriptions.isEmpty()
        || (!d->uri.isEmpty() && !d->uri.isValid()))
        return false;
    QSet<QString> languages;
    for (const auto &meta : d->meta) {
        if (meta.title.isEmpty() || languages.contains(meta.language))
            return false;
        languages.insert(meta.language);
    }
    for (const auto &description : d->descriptions) {
        if (description.isNull() || description.localName() != QLatin1String("description")
            || description.namespaceURI().isEmpty())
            return false;
    }
    return true;
}

Jid     JinglePub::from() const { return d ? d->from : Jid(); }
void    JinglePub::setFrom(const Jid &from) { ensureD()->from = from; }
QString JinglePub::id() const { return d ? d->id : QString(); }
void    JinglePub::setId(const QString &id) { ensureD()->id = id; }
QUrl    JinglePub::uri() const { return d ? d->uri : QUrl(); }
void    JinglePub::setUri(const QUrl &uri) { ensureD()->uri = uri; }

QString JinglePub::startUri() const
{
    if (!d || !d->from.isValid() || d->id.isEmpty())
        return {};
    const auto encodedJid = QUrl::toPercentEncoding(d->from.full(), QByteArray("@/"));
    const auto encodedId  = QUrl::toPercentEncoding(d->id);
    return QStringLiteral("xmpp:%1?jingle;id=%2").arg(QString::fromLatin1(encodedJid), QString::fromLatin1(encodedId));
}

bool JinglePub::parseStartUri(const QString &uri, Jid *publisher, QString *publicationId)
{
    if (!publisher || !publicationId || !uri.startsWith(QLatin1String("xmpp:"), Qt::CaseInsensitive))
        return false;

    const auto queryPos = uri.indexOf(QLatin1Char('?'), 5);
    if (queryPos < 0)
        return false;
    const auto parsedPublisher = Jid(QUrl::fromPercentEncoding(uri.mid(5, queryPos - 5).toUtf8()));
    if (!parsedPublisher.isValid())
        return false;

    const auto queryParts = uri.mid(queryPos + 1).split(QLatin1Char(';'));
    if (queryParts.isEmpty() || queryParts.first() != QLatin1String("jingle"))
        return false;
    QString parsedId;
    for (auto it = std::next(queryParts.cbegin()); it != queryParts.cend(); ++it) {
        if (it->startsWith(QLatin1String("id="))) {
            parsedId = QUrl::fromPercentEncoding(it->mid(3).toUtf8());
            break;
        }
    }
    if (parsedId.isEmpty())
        return false;

    *publisher     = parsedPublisher;
    *publicationId = parsedId;
    return true;
}

QList<JinglePub::Meta> JinglePub::meta() const { return d ? d->meta : QList<Meta>(); }
void                   JinglePub::setMeta(const QList<Meta> &meta) { ensureD()->meta = meta; }
void                   JinglePub::addMeta(const Meta &meta) { ensureD()->meta.append(meta); }
QList<QDomElement>     JinglePub::descriptions() const { return d ? d->descriptions : QList<QDomElement>(); }
void JinglePub::setDescriptions(const QList<QDomElement> &descriptions) { ensureD()->descriptions = descriptions; }
void JinglePub::addDescription(const QDomElement &description) { ensureD()->descriptions.append(description); }

void JinglePub::addDescription(const QString &applicationNamespace)
{
    if (applicationNamespace.isEmpty())
        return;
    QDomDocument doc;
    auto         element = doc.createElementNS(applicationNamespace, QStringLiteral("description"));
    doc.appendChild(element);
    ensureD()->descriptions.append(element);
}

bool JinglePub::fromXml(const QDomElement &element)
{
    if (element.isNull() || element.localName() != QLatin1String("jinglepub") || element.namespaceURI() != JINGLEPUB_NS)
        return false;

    Private parsed;
    parsed.from = Jid(element.attribute(QStringLiteral("from")));
    parsed.id   = element.attribute(QStringLiteral("id"));
    if (!parsed.from.isValid() || parsed.id.isEmpty())
        return false;

    QSet<QString> metaLanguages;
    bool          uriSeen = false;
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI() == JINGLEPUB_NS && child.localName() == QLatin1String("uri")) {
            if (uriSeen)
                return false;
            uriSeen    = true;
            parsed.uri = QUrl(child.text(), QUrl::StrictMode);
            if (!parsed.uri.isValid())
                return false;
        } else if (child.namespaceURI() == JINGLEPUB_NS && child.localName() == QLatin1String("meta")) {
            Meta meta;
            meta.language
                = child.attributeNS(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("lang"));
            meta.title   = child.attribute(QStringLiteral("title"));
            meta.summary = child.attribute(QStringLiteral("summary"));
            if (meta.title.isEmpty() || metaLanguages.contains(meta.language))
                return false;
            metaLanguages.insert(meta.language);
            parsed.meta.append(meta);
        } else if (child.localName() == QLatin1String("description") && child.namespaceURI() != JINGLEPUB_NS
                   && !child.namespaceURI().isEmpty()) {
            parsed.descriptions.append(child);
        }
    }
    if (parsed.descriptions.isEmpty())
        return false;

    d = new Private(parsed);
    return true;
}

QDomElement JinglePub::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};

    auto root = doc->createElementNS(JINGLEPUB_NS, QStringLiteral("jinglepub"));
    root.setAttribute(QStringLiteral("from"), d->from.full());
    root.setAttribute(QStringLiteral("id"), d->id);

    if (d->uri.isValid() && !d->uri.isEmpty()) {
        auto uri = doc->createElementNS(JINGLEPUB_NS, QStringLiteral("uri"));
        uri.appendChild(doc->createTextNode(d->uri.toString()));
        root.appendChild(uri);
    }
    for (const auto &meta : d->meta) {
        if (meta.title.isEmpty())
            continue;
        auto element = doc->createElementNS(JINGLEPUB_NS, QStringLiteral("meta"));
        element.setAttribute(QStringLiteral("title"), meta.title);
        if (!meta.summary.isEmpty())
            element.setAttribute(QStringLiteral("summary"), meta.summary);
        if (!meta.language.isEmpty())
            element.setAttributeNS(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("xml:lang"),
                                   meta.language);
        root.appendChild(element);
    }
    for (const auto &description : d->descriptions)
        root.appendChild(doc->importNode(description, true));
    return root;
}

class JinglePubStartTask : public Task {
public:
    JinglePubStartTask(Task *parent, const Jid &publisher, const QString &publicationId) : Task(parent), to(publisher)
    {
        iq         = createIQ(doc(), "get", publisher.full(), id());
        auto start = doc()->createElementNS(JINGLEPUB_NS, QStringLiteral("start"));
        start.setAttribute(QStringLiteral("id"), publicationId);
        iq.appendChild(start);
    }

    QString sid;

protected:
    void onGo() override { send(iq); }

    bool take(const QDomElement &element) override
    {
        if (element.localName() != QLatin1String("iq") || element.attribute(QStringLiteral("id")) != id())
            return false;
        const Jid  from(element.attribute(QStringLiteral("from")));
        const bool senderMatches = to.resource().isEmpty() ? from.compare(to, false) : from.compare(to);
        if (!from.isValid() || !senderMatches)
            return false;
        const auto type = element.attribute(QStringLiteral("type"));
        if (type != QLatin1String("result") && type != QLatin1String("error"))
            return false;
        if (type == QLatin1String("error")) {
            setError(element);
            return true;
        }
        auto starting = childElementsByTagNameNS(element, JINGLEPUB_NS, QStringLiteral("starting")).item(0).toElement();
        if (starting.isNull() || (sid = starting.attribute(QStringLiteral("sid"))).isEmpty()) {
            setError(0, QStringLiteral("Invalid XEP-0358 <starting/> response"));
            return true;
        }
        setSuccess();
        return true;
    }

private:
    Jid         to;
    QDomElement iq;
};

class PublishedSessionRequest::Private {
public:
    Manager                     *manager = nullptr;
    Jid                          publisher;
    QString                      publicationId;
    QString                      sid;
    State                        state = State::Idle;
    Stanza::Error                error;
    QPointer<JinglePubStartTask> task;
};

PublishedSessionRequest::PublishedSessionRequest(Manager *manager, const Jid &publisher, const QString &publicationId,
                                                 QObject *parent) : QObject(parent), d(new Private)
{
    d->manager       = manager;
    d->publisher     = publisher;
    d->publicationId = publicationId;
}

PublishedSessionRequest::~PublishedSessionRequest() = default;
PublishedSessionRequest::State PublishedSessionRequest::state() const { return d->state; }
Jid                            PublishedSessionRequest::publisher() const { return d->publisher; }
QString                        PublishedSessionRequest::publicationId() const { return d->publicationId; }
QString                        PublishedSessionRequest::sid() const { return d->sid; }
Stanza::Error                  PublishedSessionRequest::error() const { return d->error; }

void PublishedSessionRequest::start()
{
    if (d->state != State::Idle || !d->manager || !d->publisher.isValid() || d->publicationId.isEmpty())
        return;
    d->state  = State::Pending;
    auto task = new JinglePubStartTask(d->manager->client()->rootTask(), d->publisher, d->publicationId);
    d->task   = task;
    connect(task, &Task::finished, this, [this, task]() {
        if (task->success()) {
            d->sid   = task->sid;
            d->state = State::Succeeded;
        } else {
            d->error = task->error();
            d->state = State::Failed;
        }
        emit finished();
    });
    task->go(true);
}

} // namespace XMPP::Jingle
