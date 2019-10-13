/*
 * subsets.cpp - Implementation of Result Set Management (XEP-0059)
 * Copyright (C) 2018  Aleksey Andreev
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

#include "xmpp_subsets.h"

#include "xmpp_xmlcommon.h"

using namespace XMPP;

static QLatin1String xmlns_ns_rsm("http://jabber.org/protocol/rsm");

class SubsetsClientManager::Private {
public:
    enum QueryType { None, Count, First, Last, Next, Previous, Index };
    struct {
        QueryType type;
        int       max;
        int       index;
    } query;
    struct {
        int     count;
        int     index;
        bool    first;
        bool    last;
        int     itemsCount;
        QString firstId;
        QString lastId;
    } result;
    bool valid;

    void resetResult()
    {
        result.count      = -1;
        result.index      = -1;
        result.first      = false;
        result.last       = false;
        result.itemsCount = 0;
        valid             = false;
    }

    QDomElement mainElement(QDomDocument *doc)
    {
        QDomElement e = doc->createElementNS(xmlns_ns_rsm, QStringLiteral("set"));
        return e;
    }

    void insertMaxElement(QDomDocument *doc, QDomElement *el, int i)
    {
        el->appendChild(textTag(doc, QStringLiteral("max"), QString::number(i)));
    }

    void insertBeforeElement(QDomDocument *doc, QDomElement *el, const QString &s)
    {
        if (s.isEmpty())
            el->appendChild(doc->createElement(QStringLiteral("before")));
        else
            el->appendChild(textTag(doc, QStringLiteral("before"), s));
    }

    void insertAfterElement(QDomDocument *doc, QDomElement *el, const QString &s)
    {
        el->appendChild(textTag(doc, QStringLiteral("after"), s));
    }

    void insertIndexElement(QDomDocument *doc, QDomElement *el, int i)
    {
        el->appendChild(textTag(doc, QStringLiteral("index"), QString::number(i)));
    }

    bool updateFromElement(const QDomElement &el)
    {
        valid          = true;
        bool        ok = false;
        QDomElement e  = el.firstChildElement(QLatin1String("count"));
        if (!e.isNull())
            result.count = tagContent(e).toInt(&ok);
        if (!ok || result.count < 0)
            result.count = -1;

        result.index = -1;
        e            = el.firstChildElement(QLatin1String("first"));
        if (!e.isNull()) {
            result.firstId = tagContent(e);
            if (result.firstId.isEmpty())
                valid = false;
            int i = e.attribute(QLatin1String("index")).toInt(&ok);
            if (ok && i >= 0)
                result.index = i;
        } else
            result.firstId = "";

        e = el.firstChildElement(QLatin1String("last"));
        if (!e.isNull()) {
            result.lastId = tagContent(e);
            if (result.lastId.isEmpty())
                valid = false;
        } else
            result.lastId = "";

        if (result.firstId.isEmpty() != result.lastId.isEmpty())
            valid = false;

        result.first = query.type == First || result.index == 0
            || (result.itemsCount == 0 && result.index == -1 && (query.type == Last || query.type == Previous));
        result.last = query.type == Last
            || (result.index != -1 && result.count != -1 && result.count - result.itemsCount <= result.index)
            || (result.itemsCount == 0 && result.index == -1 && (query.type == First || query.type == Next));
        if (result.firstId.isEmpty() && result.lastId.isEmpty()) {
            switch (query.type) {
            case Previous:
                result.first = true;
                break;
            case Next:
            case Index:
                result.last = true;
                break;
            default:
                break;
            }
        }
        return valid;
    }
};

SubsetsClientManager::SubsetsClientManager()
{
    d = new Private;
    reset();
}

SubsetsClientManager::~SubsetsClientManager() { delete d; }

void SubsetsClientManager::reset()
{
    d->query.type     = Private::None;
    d->query.max      = 50;
    d->query.index    = -1;
    d->result.firstId = QString();
    d->result.lastId  = QString();
    d->resetResult();
}

bool SubsetsClientManager::isValid() const { return d->valid; }

bool SubsetsClientManager::isFirst() const { return d->result.first; }

bool SubsetsClientManager::isLast() const { return d->result.last; }

int SubsetsClientManager::count() const { return d->result.count; }

void SubsetsClientManager::setMax(int max) { d->query.max = max; }

QDomElement SubsetsClientManager::findElement(const QDomElement &el, bool child)
{
    if (el.tagName() == QLatin1String("set") && el.namespaceURI() == xmlns_ns_rsm)
        return el;

    if (child) {
        QDomElement e = el.firstChildElement(QLatin1String("set"));
        while (!e.isNull()) {
            if (e.namespaceURI() == xmlns_ns_rsm) {
                return e;
            }
            e = e.nextSiblingElement(QLatin1String("set"));
        }
    }
    return QDomElement();
}

bool SubsetsClientManager::updateFromElement(const QDomElement &el, int itemsCount)
{
    if (findElement(el, false).isNull())
        return false;

    d->result.itemsCount = itemsCount;
    return d->updateFromElement(el);
}

void SubsetsClientManager::getCount()
{
    d->query.type = Private::Count;
    d->resetResult();
}

void SubsetsClientManager::getFirst()
{
    d->query.type = Private::First;
    d->resetResult();
}

void SubsetsClientManager::getNext()
{
    d->query.type = Private::Next;
    d->resetResult();
}

void SubsetsClientManager::getLast()
{
    d->query.type = Private::Last;
    d->resetResult();
}

void SubsetsClientManager::getPrevious()
{
    d->query.type = Private::Previous;
    d->resetResult();
}

void SubsetsClientManager::getByIndex()
{
    d->query.type = Private::Index;
    d->resetResult();
}

QDomElement SubsetsClientManager::makeQueryElement(QDomDocument *doc) const
{
    if (d->query.type == Private::None)
        return QDomElement();

    QDomElement e = d->mainElement(doc);
    switch (d->query.type) {
    case Private::Count:
        d->insertMaxElement(doc, &e, 0);
        break;
    case Private::Last:
        d->insertBeforeElement(doc, &e, QString());
        break;
    case Private::Next:
        d->insertAfterElement(doc, &e, d->result.lastId);
        break;
    case Private::Previous:
        d->insertBeforeElement(doc, &e, d->result.firstId);
        break;
    case Private::Index:
        d->insertIndexElement(doc, &e, d->query.index);
    case Private::First:
    default:
        break;
    }
    if (d->query.type != Private::Count)
        d->insertMaxElement(doc, &e, d->query.max);

    return e;
}
