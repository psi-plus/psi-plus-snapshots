/*
 * jingle-pub.h - XEP-0358 Publishing Available Jingle Sessions
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XMPP_JINGLE_PUB_H
#define XMPP_JINGLE_PUB_H

#include <iris/iris_export.h>

#include <iris/xmpp-core/xmpp_stanza.h>

#include <QDomElement>
#include <QObject>
#include <QSharedDataPointer>
#include <QUrl>

#include <memory>

namespace XMPP::Jingle {

extern IRIS_EXPORT const QString JINGLEPUB_NS;

class JinglePub {
public:
    struct Meta {
        QString language;
        QString title;
        QString summary;
    };

    JinglePub();
    explicit JinglePub(const QDomElement &element);
    JinglePub(const JinglePub &);
    JinglePub &operator=(const JinglePub &);
    ~JinglePub();

    bool isValid() const;

    Jid  from() const;
    void setFrom(const Jid &from);

    QString id() const;
    void    setId(const QString &id);

    QUrl uri() const;
    void setUri(const QUrl &uri);

    QString     startUri() const;
    static bool parseStartUri(const QString &uri, Jid *publisher, QString *publicationId);

    QList<Meta> meta() const;
    void        setMeta(const QList<Meta> &meta);
    void        addMeta(const Meta &meta);

    QList<QDomElement> descriptions() const;
    void               setDescriptions(const QList<QDomElement> &descriptions);
    void               addDescription(const QDomElement &description);
    void               addDescription(const QString &applicationNamespace);

    bool        fromXml(const QDomElement &element);
    QDomElement toXml(QDomDocument *doc) const;

private:
    class Private;
    Private                    *ensureD();
    QSharedDataPointer<Private> d;
};

class IRIS_EXPORT PublishedSessionRequest : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Pending, Succeeded, Failed };

    ~PublishedSessionRequest() override;

    State         state() const;
    Jid           publisher() const;
    QString       publicationId() const;
    QString       sid() const;
    Stanza::Error error() const;

    void start();

signals:
    void finished();

private:
    friend class Manager;
    PublishedSessionRequest(class Manager *manager, const Jid &publisher, const QString &publicationId,
                            QObject *parent = nullptr);

    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP::Jingle

#endif // XMPP_JINGLE_PUB_H
