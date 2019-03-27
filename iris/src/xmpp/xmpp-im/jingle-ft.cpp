/*
 * jignle-ft.h - Jingle file transfer
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

#include "jingle-ft.h"
#include "xmpp_client.h"
#include "xmpp_thumbs.h"
#include "xmpp_hash.h"
#include "xmpp_xmlcommon.h"

namespace XMPP {
namespace Jingle {

namespace FileTransfer {

const QString NS = QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5");


QDomElement Range::toXml(QDomDocument *doc) const
{
    auto r = doc->createElement(QStringLiteral("range"));
    if (length) {
        r.setAttribute(QStringLiteral("length"), QString::number(length));
    }
    if (offset) {
        r.setAttribute(QStringLiteral("length"), QString::number(length));
    }
    auto h = hash.toXml(doc);
    if (!h.isNull()) {
        r.appendChild(h);
    }
    return r;
}

//----------------------------------------------------------------------------
// File
//----------------------------------------------------------------------------
class File::Private : public QSharedData
{
public:
    QDateTime date;
    QString   mediaType;
    QString   name;
    QString   desc;
    quint64   size = 0;
    Range     range;
    bool      rangeSupported = false;
    Hash      hash;
    Thumbnail thumbnail;
};

File::File()
{

}

File::~File()
{

}

File::File(const File &other) :
    d(other.d)
{

}

File::File(const QDomElement &file)
{
    QDateTime date;
    QString   mediaType;
    QString   name;
    QString   desc;
    size_t    size = 0;
    Range     range;
    Hash      hash;
    Thumbnail thumbnail;

    bool ok;

    for(QDomElement ce = file.firstChildElement();
        !ce.isNull(); ce = ce.nextSiblingElement()) {

        if (ce.tagName() == QLatin1String("date")) {
            date = QDateTime::fromString(ce.text().left(19), Qt::ISODate);
            if (!date.isValid()) {
                return;
            }

        } else if (ce.tagName() == QLatin1String("media-type")) {
            mediaType = ce.text();

        } else if (ce.tagName() == QLatin1String("name")) {
            name = ce.text();

        } else if (ce.tagName() == QLatin1String("size")) {
            size = ce.text().toULongLong(&ok);
            if (!ok) {
                return;
            }

        } else if (ce.tagName() == QLatin1String("range")) {
            if (ce.hasAttribute(QLatin1String("offset"))) {
                range.offset = ce.attribute(QLatin1String("offset")).toULongLong(&ok);
                if (!ok) {
                    return;
                }
            }
            if (ce.hasAttribute(QLatin1String("length"))) {
                range.offset = ce.attribute(QLatin1String("length")).toULongLong(&ok);
                if (!ok) {
                    return;
                }
            }
            QDomElement hashEl = ce.firstChildElement(QLatin1String("hash"));
            if (hashEl.namespaceURI() == QLatin1String("urn:xmpp:hashes:2")) {
                range.hash = Hash(hashEl);
                if (range.hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }
            d->rangeSupported = true;

        } else if (ce.tagName() == QLatin1String("desc")) {
            desc = ce.text();

        } else if (ce.tagName() == QLatin1String("hash")) {
            if (ce.namespaceURI() == QLatin1String(XMPP_HASH_NS)) {
                hash = Hash(ce);
                if (hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

        } else if (ce.tagName() == QLatin1String("hash-used")) {
            if (ce.namespaceURI() == QLatin1String(XMPP_HASH_NS)) {
                hash = Hash(ce);
                if (hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

        } else if (ce.tagName() == QLatin1String("thumbnail")) {
            thumbnail = Thumbnail(ce);
        }
    }

    auto p = new Private;
    p->date = date;
    p->mediaType = mediaType;
    p->name = name;
    p->desc = desc;
    p->size = size;
    p->range = range;
    p->hash = hash;
    p->thumbnail = thumbnail;

    d = p;
}

QDomElement File::toXml(QDomDocument *doc) const
{
    if (!isValid()) {
        return QDomElement();
    }
    QDomElement el = doc->createElement(QStringLiteral("file"));
    if (d->date.isValid()) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("date"), d->date.toString(Qt::ISODate)));
    }
    if (d->desc.size()) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("desc"), d->desc));
    }
    if (d->hash.isValid()) {
        el.appendChild(d->hash.toXml(doc));
    }
    if (d->mediaType.size()) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("media-type"), d->mediaType));
    }
    if (d->name.size()) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("name"), d->name));
    }
    if (d->size) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("size"), QString::number(d->size)));
    }
    if (d->rangeSupported || d->range.isValid()) {
        el.appendChild(d->range.toXml(doc));
    }
    if (d->thumbnail.isValid()) {
        el.appendChild(d->thumbnail.toXml(doc));
    }
    return el;
}

QDateTime File::date() const
{
    return d? d->date : QDateTime();
}

QString File::description() const
{
    return d? d->desc : QString();
}

Hash File::hash() const
{
    return d? d->hash : Hash();
}

QString File::mediaType() const
{
    return d? d->mediaType : QString();
}

QString File::name() const
{
    return d? d->name : QString();
}

quint64 File::size() const
{
    return d? d->size : 0;
}

Range File::range() const
{
    return d? d->range : Range();
}

Thumbnail File::thumbnail() const
{
    return d? d->thumbnail: Thumbnail();
}

void File::setDate(const QDateTime &date)
{
    ensureD()->date = date;
}

void File::setDescription(const QString &desc)
{
    ensureD()->desc = desc;
}

void File::setHash(const Hash &hash)
{
    ensureD()->hash = hash;
}

void File::setMediaType(const QString &mediaType)
{
    ensureD()->mediaType = mediaType;
}

void File::setName(const QString &name)
{
    ensureD()->name = name;
}

void File::setSize(quint64 size)
{
    ensureD()->size = size;
}

void File::setRange(const Range &range)
{
    ensureD()->range = range;
    d->rangeSupported = true;
}

void File::setThumbnail(const Thumbnail &thumb)
{
    ensureD()->thumbnail = thumb;
}

File::Private *File::ensureD()
{
    if (!d) {
        d = new Private;
    }
    return d.data();
}

//----------------------------------------------------------------------------
// Checksum
//----------------------------------------------------------------------------
Checksum::Checksum(const QDomElement &cs) :
    ContentBase(cs)
{
    file = File(cs.firstChildElement(QLatin1String("file")));
}

bool Checksum::isValid() const
{
    return ContentBase::isValid() && file.isValid();
}

QDomElement Checksum::toXml(QDomDocument *doc) const
{
    auto el = ContentBase::toXml(doc, "checksum");
    if (!el.isNull()) {
        el.appendChild(file.toXml(doc));
    }
    return el;
}

//----------------------------------------------------------------------------
// Received
//----------------------------------------------------------------------------
QDomElement Received::toXml(QDomDocument *doc) const
{
    return ContentBase::toXml(doc, "received");
}

//----------------------------------------------------------------------------
// ApplicationManager
//----------------------------------------------------------------------------
Manager::Manager(QObject *parent):
    XMPP::Jingle::ApplicationManager(parent)
{

}

void Manager::setJingleManager(XMPP::Jingle::Manager *jm)
{
    jingleManager = jm;
}

Application* Manager::startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName, Origin creator, Origin senders)
{
    if (!(contentName.size() > 0 && (senders == Origin::Initiator || senders == Origin::Responder))) {
        qDebug("Invalid Jignle FT App start parameters");
        return nullptr;
    }
    return new Application(pad.staticCast<Pad>(), contentName, creator, senders); // ContentOrigin::Remote
}

ApplicationManagerPad* Manager::pad(Session *session)
{
    return new Pad(this, session);
}

void Manager::closeAll()
{

}

Client *Manager::client()
{
    if (jingleManager) {
        return jingleManager->client();
    }
    return nullptr;
}

QStringList Manager::availableTransports() const
{
    return jingleManager->availableTransports(Transport::Reliable);
}

//----------------------------------------------------------------------------
// Application
//----------------------------------------------------------------------------
class Application::Private
{
public:

    State   state = State::Created;
    QSharedPointer<Pad> pad;
    QString contentName;
    File    file;
    Origin  creator;
    Origin  senders;
    QSharedPointer<Transport> transport;
    QStringList availableTransports;
};

Application::Application(const QSharedPointer<Pad> &pad, const QString &contentName, Origin creator, Origin senders) :
    d(new Private)
{
    d->pad     = pad;
    d->contentName = contentName;
    d->creator = creator;
    d->senders = senders;
    d->availableTransports = static_cast<Manager*>(pad->manager())->availableTransports();
}

Application::~Application()
{

}

ApplicationManagerPad::Ptr Application::pad() const
{
    return d->pad.staticCast<ApplicationManagerPad>();
}

State Application::state() const
{
    return d->state;
}

QString Application::contentName() const
{
    return d->contentName;
}

Origin Application::creator() const
{
    return d->creator;
}

Origin Application::senders() const
{
   return d->senders;
}

Application::SetDescError Application::setDescription(const QDomElement &description)
{
    d->file = File(description.firstChildElement("file"));
    d->state = State::Pending; // basically it's incomming  content. so if we parsed it it's pending. if not parsed if will rejected anyway.
    return d->file.isValid()? Ok: Unparsed;
}

void Application::setFile(const File &file)
{
    d->file = file;
}

// incoming one? or we have to check real direction
bool Application::setTransport(const QSharedPointer<Transport> &transport)
{
    if (transport->features() & Transport::Reliable) {
        int nsIndex = d->availableTransports.indexOf(transport->pad()->ns());
        if (nsIndex == -1) {
            return false;
        }
        d->availableTransports.removeAt(nsIndex);
        d->transport = transport;
        d->transport->setApplication(this);
        d->state = State::Pending;
        return true;
    }
    return false;
}

QSharedPointer<Transport> Application::transport() const
{
    return d->transport;
}

Action Application::outgoingUpdateType() const
{
    switch (d->state) {
    case State::Created:
        if (!d->transport && !d->availableTransports.size()) {
            break; // not yet ready
        }
        return Action::ContentAdd;
    case State::Connecting:
    case State::Active:
        return d->transport->outgoingUpdateType();
    case State::Pending:
    default:
        break;
    }
    return Action::NoAction; // TODO
}

bool Application::isReadyForSessionAccept() const
{
    return d->state == State::Pending; // check direction as well?
}

QDomElement Application::takeOutgoingUpdate()
{
    if (d->state == State::Connecting || d->state == State::Active) {
        return d->transport->takeOutgoingUpdate();
    }
    if (d->state == State::Created && isValid()) { // basically when we come to this function Created is possible only for outgoing content
        if (!d->transport && d->availableTransports.size()) {
            selectNextTransport();
        }
        if (!d->transport) { // failed to select next transport. can't continue
            return QDomElement();
        }
        auto client = d->pad->session()->manager()->client();
        if (d->file.thumbnail().data.size()) {
            auto thumb = d->file.thumbnail();
            auto bm = client->bobManager();
            BoBData data = bm->append(thumb.data, thumb.mimeType);
            thumb.uri = QLatin1String("cid:") + data.cid();
            d->file.setThumbnail(thumb);
        }
        auto doc = client->doc();
        ContentBase cb(d->pad->session()->role(), d->contentName);
        cb.senders = d->senders;
        auto cel = cb.toXml(doc, "content");
        cel.appendChild(doc->createElementNS(NS, "description")).appendChild(d->file.toXml(doc));
        cel.appendChild(d->transport->takeOutgoingUpdate());
        return cel;
    }
    return QDomElement(); // TODO
}

QDomElement Application::sessionAcceptContent() const
{
    return QDomElement(); // TODO
}

bool Application::wantBetterTransport(const QSharedPointer<Transport> &t) const
{
    Q_UNUSED(t)
    return true; // TODO check
}

bool Application::selectNextTransport()
{
    if (d->availableTransports.size()) {
        QString ns = d->availableTransports.takeFirst();
        d->transport = d->pad->session()->newOutgoingTransport(ns);
        d->transport->setApplication(this);
        return true;
    }
    return false;
}

void Application::prepare()
{
    if (!d->transport) {
        selectNextTransport();
    }
    if (d->transport) {
        d->transport->prepare();
    }
}

bool Application::isValid() const
{
    return d->file.isValid() && d->transport &&  d->contentName.size() > 0 &&
            (d->senders == Origin::Initiator || d->senders == Origin::Responder);
}

Pad::Pad(Manager *manager, Session *session) :
    _manager(manager),
    _session(session)
{

}

QDomElement Pad::takeOutgoingSessionInfoUpdate()
{
    return QDomElement(); // TODO
}

QString Pad::ns() const
{
    return NS;
}

Session *Pad::session() const
{
    return _session;
}

ApplicationManager *Pad::manager() const
{
    return _manager;
}

QString Pad::generateContentName(Origin senders)
{
    QString prefix = senders == _session->role()? "fileoffer" : "filereq";
    QString name;
    do {
        name = prefix + QString("_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
    } while (_session->content(name, _session->role()));
    return name;
}

void Pad::addOutgoingOffer(const File &file)
{
    auto selfp = _session->applicationPad(NS);
    auto app = _manager->startApplication(selfp, "ft", _session->role(), _session->role());
    app->setFile(file);
}



} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP
