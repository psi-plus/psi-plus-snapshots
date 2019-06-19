/*
 * xmpp_reference.h - XMPP References / XEP-0372
 * Copyright (C) 2019  Sergey Ilinykh
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

#include "xmpp_reference.h"

using namespace XMPP;

#define D() (d? d:(d=new Private))

const QString XMPP::MEDIASHARING_NS(QStringLiteral("urn:xmpp:sims:1"));
const QString XMPP::REFERENCE_NS(QStringLiteral("urn:xmpp:reference:0"));

class Reference::Private : public QSharedData
{
public:
    Reference::Type type;
    QString uri;
    QString anchor;
    int     begin = -1;
    int     end   = -1;
    MediaSharing mediaSharing;
};

Reference::Reference()
{

}

Reference::Reference(Type type, const QString &uri) :
    d(new Private)
{
    d->type = type;
    d->uri = uri;
}

Reference::~Reference()
{

}

Reference::Reference(const Reference &other) :
    d(other.d)
{

}

Reference &Reference::operator=(const Reference &other)
{
    d = other.d;
    return *this;
}

Reference::Type Reference::type() const
{
    return d->type;
}

const QString &Reference::uri() const
{
    return d->uri;
}

void Reference::setRange(int begin, int end)
{
    D()->begin = begin;
    d->end = end;
}

int Reference::start() const
{
    return d->begin;
}

int Reference::end() const
{
    return d->end;
}

const QString &Reference::anchor() const
{
    return d->anchor;
}

void Reference::setAnchor(const QString &anchor)
{
    D()->anchor = anchor;
}

void Reference::setMediaSharing(const MediaSharing &ms)
{
    D()->mediaSharing = ms;
}

const MediaSharing &Reference::mediaSharing() const
{
    return d->mediaSharing;
}

bool Reference::fromXml(const QDomElement &e)
{
    QString type = e.attribute(QString::fromLatin1("type"));
    QString uri = e.attribute(QString::fromLatin1("uri"));
    QString begin = e.attribute(QString::fromLatin1("begin"));
    QString end = e.attribute(QString::fromLatin1("end"));
    QString anchor = e.attribute(QString::fromLatin1("anchor"));

    if (type.isEmpty() || uri.isEmpty()) {
        return false;
    }

    Type t;
    if (type == QString::fromLatin1("data"))
        t = Data;
    else if (type == QString::fromLatin1("mention"))
        t = Mention;
    else
        return false;

    int beginN = -1, endN = -1;
    bool ok;
    if (!begin.isEmpty() && !(beginN = begin.toInt(&ok),ok)) {
        return false;
    }
    if (!end.isEmpty() && !(endN = end.toInt(&ok),ok)) {
        return false;
    }

    auto msEl = e.firstChildElement("media-sharing");
    MediaSharing ms;
    if (msEl.attribute(QString::fromLatin1("xmlns")) == MEDIASHARING_NS) {
        auto fileEl = msEl.firstChildElement("file");
        auto sourcesEl = msEl.firstChildElement("file");
        if (sourcesEl.isNull() || fileEl.isNull() || fileEl.attribute(QString::fromLatin1("xmlns")) != XMPP::Jingle::FileTransfer::NS)
            return false;

        ms.file = XMPP::Jingle::FileTransfer::File(fileEl);
        if (!ms.file.isValid())
            return false;


        auto srcName = QString::fromLatin1("reference");
        for (auto el = msEl.firstChildElement(srcName); !el.isNull(); el = el.nextSiblingElement(srcName)) {
            if (el.attribute(QString::fromLatin1("xmlns")) == REFERENCE_NS) {
                Reference ref;
                if (!ref.fromXml(el)) {
                    return false;
                }
                ms.sources.append(ref.uri());
            }
        }
    }

    D()->type = t;
    d->uri = uri;
    d->begin = beginN;
    d->end = endN;
    d->anchor = anchor;
    d->mediaSharing = ms;

    return true;
}

QDomElement Reference::toXml(QDomDocument *doc) const
{
    if (!d) {
        return QDomElement();
    }
    auto root = doc->createElementNS(REFERENCE_NS, QString::fromLatin1("reference"));
    root.setAttribute(QString::fromLatin1("uri"), d->uri);
    root.setAttribute(QString::fromLatin1("type"), QString(d->type == Reference::Mention? "mention": "data"));

    if (d->mediaSharing.file.isValid() && d->mediaSharing.sources.count()) {
        auto msEl = doc->createElementNS(MEDIASHARING_NS, QString::fromLatin1("media-sharing"));
        root.appendChild(msEl);
        msEl.appendChild(d->mediaSharing.file.toXml(doc));
        auto sourcesEl = msEl.appendChild(doc->createElement(QString::fromLatin1("sources"))).toElement();
        for (auto const &s: d->mediaSharing.sources) {
            auto sEl = sourcesEl.appendChild(doc->createElementNS(REFERENCE_NS, QString::fromLatin1("reference"))).toElement();
            sEl.setAttribute(QString::fromLatin1("uri"), s);
            sEl.setAttribute(QString::fromLatin1("type"), QString::fromLatin1("data"));
        }
    }

    if (d->begin != -1)
        root.setAttribute(QString::fromLatin1("begin"), d->begin);

    if (d->end != -1)
        root.setAttribute(QString::fromLatin1("end"), d->end);

    if (!d->anchor.isEmpty())
        root.setAttribute(QString::fromLatin1("anchor"), d->anchor);

    return root;
}

