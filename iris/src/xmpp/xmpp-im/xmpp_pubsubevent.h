/*
 * xmpp_pubsubevent.h - XEP-0060 event value type
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef XMPP_PUBSUBEVENT_H
#define XMPP_PUBSUBEVENT_H

#include "xmpp_pubsubitem.h"
#include "xmpp_pubsubretraction.h"

#include <QDomElement>
#include <QList>
#include <QString>

namespace XMPP {

/** A direct child of an XEP-0060 <event/> notification.
 *
 * Known event forms have a typed representation while element() preserves the
 * original XML for fields which are not modeled yet and for future XEP-0060
 * extensions. Unknown direct children are retained with Type::Unknown.
 */
class PubSubEvent {
public:
    enum class Type { Items, Collection, Configuration, Delete, Purge, Subscription, Unknown };

    PubSubEvent();
    PubSubEvent(Type type, const QString &node, const QList<PubSubItem> &items,
                const QList<PubSubRetraction> &retractions, const QDomElement &element);

    Type                           type() const;
    const QString                 &node() const;
    const QList<PubSubItem>       &items() const;
    const QList<PubSubRetraction> &retractions() const;
    const QDomElement             &element() const;

private:
    Type                    type_ = Type::Unknown;
    QString                 node_;
    QList<PubSubItem>       items_;
    QList<PubSubRetraction> retractions_;
    QDomElement             element_;
};

} // namespace XMPP

#endif // XMPP_PUBSUBEVENT_H
