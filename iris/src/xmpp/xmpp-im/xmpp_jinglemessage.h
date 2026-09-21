/*
 * xmpp_jinglemessage.h - XEP-0353 Jingle Message Initiation value type
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#ifndef XMPP_JINGLEMESSAGE_H
#define XMPP_JINGLEMESSAGE_H

#include <iris/iris_export.h>

#include <QDomElement>
#include <QList>
#include <QSharedDataPointer>
#include <QString>

#include <any>
#include <functional>
#include <optional>

class QDomDocument;

namespace XMPP { namespace Jingle {

class IRIS_EXPORT MessageInitiation {
public:
    enum class Action { None, Propose, Ringing, Proceed, Reject, Retract, Finish };

    struct Description {
        QString  applicationNamespace;
        std::any data;

        bool isValid() const;
        bool isSupported() const { return data.has_value(); }
    };

    using DescriptionParser = std::function<std::optional<std::any>(const QDomElement &)>;
    using DescriptionSerializer
        = std::function<QDomElement(const QString &, const std::any &, QDomDocument *)>;

    MessageInitiation();
    MessageInitiation(Action action, const QString &id);
    MessageInitiation(const MessageInitiation &);
    MessageInitiation &operator=(const MessageInitiation &);
    ~MessageInitiation();

    static const QString &ns();
    static MessageInitiation fromXml(const QDomElement &element, const DescriptionParser &parser = {});

    bool    isValid() const;
    Action  action() const;
    QString id() const;

    // The JMI envelope only retains the application namespace and a payload
    // produced by the matching ApplicationManager. Unknown applications remain
    // visible as unsupported descriptions with an empty std::any.
    QList<Description> descriptions() const;
    void setDescriptions(const QList<Description> &descriptions);
    void addDescription(const QString &applicationNamespace, std::any data = {});

    QString reasonCondition() const;
    QString reasonText() const;
    void    setReason(const QString &condition, const QString &text = QString());

    bool tieBreak() const;
    void setTieBreak(bool enabled);

    QString migratedTo() const;
    void setMigratedTo(const QString &id);

    QDomElement toXml(QDomDocument *doc, const DescriptionSerializer &serializer = {}) const;

private:
    class Private;
    Private *ensureD();

    QSharedDataPointer<Private> d;
};

}} // namespace XMPP::Jingle

#endif // XMPP_JINGLEMESSAGE_H
