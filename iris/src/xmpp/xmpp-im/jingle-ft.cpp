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
const QString SPECTRUM_NS = QStringLiteral("iris.psi-im.org/spectrum");

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
    QList<Hash> hashes;
    Thumbnail thumbnail;
    File::Spectrum audioSpectrum;
};

File::File()
{

}

File::~File()
{

}

File &File::operator=(const File &other)
{
    d = other.d;
    return *this;
}

File::File(const File &other) :
    d(other.d)
{

}

File::File(const QDomElement &file)
{
    QDateTime   date;
    QString     mediaType;
    QString     name;
    QString     desc;
    size_t      size = 0;
    bool        rangeSupported = false;
    Range       range;
    QList<Hash> hashes;
    Thumbnail   thumbnail;
    Spectrum    spectrum;

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
            if (hashEl.namespaceURI() == HASH_NS || hashEl.namespaceURI() == HASH_NS) {
                range.hash = Hash(hashEl);
                if (range.hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }
            rangeSupported = true;

        } else if (ce.tagName() == QLatin1String("desc")) {
            desc = ce.text();

        } else if (ce.tagName() == QLatin1String("hash")) {
            if (ce.namespaceURI() == HASH_NS || ce.namespaceURI() == HASH_NS) {
                Hash h(ce);
                if (h.type() == Hash::Type::Unknown) {
                    return;
                }
                hashes.append(h);
            }

        } else if (ce.tagName() == QLatin1String("hash-used")) {
            if (ce.namespaceURI() == HASH_NS || ce.namespaceURI() == HASH_NS) {
                Hash h(ce);
                if (h.type() == Hash::Type::Unknown) {
                    return;
                }
                hashes.append(h);
            }

        } else if (ce.tagName() == QLatin1String("thumbnail")) {
            thumbnail = Thumbnail(ce);
        } else if (ce.tagName() == QLatin1String("spectrum") && (ce.namespaceURI() == SPECTRUM_NS || ce.namespaceURI() == SPECTRUM_NS)) {
            QStringList spv = ce.text().split(',');
            spectrum.bars.reserve(spv.count());
            std::transform(spv.begin(), spv.end(), std::back_inserter(spectrum.bars), [](const QString &v){ return v.toUInt(); });
            auto c = ce.attribute(QStringLiteral("coding")).toLatin1();
            if (c == "u8")
                spectrum.coding = File::Spectrum::Coding::U8;
            else if (c == "s8")
                spectrum.coding = File::Spectrum::Coding::S8;
            else if (c == "u16")
                spectrum.coding = File::Spectrum::Coding::U16;
            else if (c == "s16")
                spectrum.coding = File::Spectrum::Coding::S16;
            else if (c == "u32")
                spectrum.coding = File::Spectrum::Coding::U32;
            else if (c == "s32")
                spectrum.coding = File::Spectrum::Coding::S32;
            else
                spectrum.bars.clear(); // drop invalid spectrum
        }
    }

    auto p = new Private;
    p->date = date;
    p->mediaType = mediaType;
    p->name = name;
    p->desc = desc;
    p->size = size;
    p->rangeSupported = rangeSupported;
    p->range = range;
    p->hashes = hashes;
    p->thumbnail = thumbnail;
    p->audioSpectrum = spectrum;

    d = p;
}

QDomElement File::toXml(QDomDocument *doc) const
{
    if (!isValid() || d->hashes.isEmpty()) {
        return QDomElement();
    }
    QDomElement el = doc->createElementNS(NS, QStringLiteral("file"));
    if (d->date.isValid()) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("date"), d->date.toString(Qt::ISODate)));
    }
    if (d->desc.size()) {
        el.appendChild(XMLHelper::textTag(*doc, QStringLiteral("desc"), d->desc));
    }
    for (const auto &h: d->hashes) {
        el.appendChild(h.toXml(doc));
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
    if (d->audioSpectrum.bars.count()) {
        auto sel = el.appendChild(doc->createElementNS(SPECTRUM_NS, QString::fromLatin1("spectrum"))).toElement();
        const char* s[] = {"u8","s8","u16","s16","u32","s32"};
        sel.setAttribute(QString::fromLatin1("coding"), QString::fromLatin1(s[d->audioSpectrum.coding]));
        QStringList sl;
        std::transform(d->audioSpectrum.bars.begin(), d->audioSpectrum.bars.end(), std::back_inserter(sl),
                       [this](quint32 v){
            switch (d->audioSpectrum.coding) {
            case Spectrum::U8:  return QString::number(quint8(v));
            case Spectrum::S8:  return QString::number(qint8(v));
            case Spectrum::U16: return QString::number(quint16(v));
            case Spectrum::S16: return QString::number(qint16(v));
            case Spectrum::U32: return QString::number(quint32(v));
            case Spectrum::S32: return QString::number(qint32(v));
            }
            return QString();
        });
        sel.appendChild(doc->createTextNode(sl.join(',')));
    }
    return el;
}

bool File::merge(const File &other)
{
    if (!d->thumbnail.isValid()) {
        d->thumbnail = other.thumbnail();
    }
    for (auto const &h: other.d->hashes) {
        auto it = std::find_if(d->hashes.constBegin(), d->hashes.constEnd(), [&h](auto const &v){ return h.type() == v.type(); });
        if (it == d->hashes.constEnd()) {
            d->hashes.append(h);
        } else if (h.data() != it->data()) {
            return false; // hashes are different
        }
    }
    return true;
}

bool File::hasComputedHashes() const
{
    if (!d)
        return false;
    for (auto const &h: d->hashes) {
        if (h.data().size())
            return true;
    }
    return false;
}

QDateTime File::date() const
{
    return d? d->date : QDateTime();
}

QString File::description() const
{
    return d? d->desc : QString();
}

Hash File::hash(Hash::Type t) const
{
    if (d && d->hashes.count()) {
        if (t == Hash::Unknown)
            return d->hashes.at(0);
        for (auto const &h: d->hashes) {
            if (h.type() == t) {
                return h;
            }
        }
    }
    return Hash();
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

File::Spectrum File::audioSpectrum() const
{
    return d? d->audioSpectrum: Spectrum();
}

void File::setDate(const QDateTime &date)
{
    ensureD()->date = date;
}

void File::setDescription(const QString &desc)
{
    ensureD()->desc = desc;
}

void File::addHash(const Hash &hash)
{
    ensureD()->hashes.append(hash);
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

Manager::~Manager()
{
    if (jingleManager)
        jingleManager->unregisterApp(NS);
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
    Application *q = nullptr;
    State   state = State::Created;
    State   transportReplaceState = State::Finished;
    Action  updateToSend = Action::NoAction;
    QSharedPointer<Pad> pad;
    QString contentName;
    File    file;
    File    acceptFile; // as it comes with "accept" response
    Origin  creator;
    Origin  senders;
    Origin  transportReplaceOrigin = Origin::None;
    XMPP::Stanza::Error lastError;
    QSharedPointer<Transport> transport;
    Connection::Ptr connection;
    QStringList availableTransports;
    bool closeDeviceOnFinish = true;
    QIODevice *device = nullptr;
    quint64 bytesLeft = 0;

    void setState(State s)
    {
        state = s;
        if (s == State::Finished) {
            if (device && closeDeviceOnFinish) {
                device->close();
            }
            if (connection) {
                connection->close();
            }
        }
        emit q->stateChanged(s);
    }

    void handleStreamFail()
    {
        // TODO d->lastError = Condition::FailedApplication
        setState(State::Finished);
    }

    void writeNextBlockToTransport()
    {
        if (!bytesLeft) {
            setState(State::Finished);
            return; // everything is written
        }
        auto sz = connection->blockSize();
        sz = sz? sz : 8192;
        if (sz > bytesLeft) {
            sz = bytesLeft;
        }
        QByteArray data = device->read(sz);
        if (data.isEmpty()) {
            handleStreamFail();
            return;
        }
        if (connection->write(data) == -1) {
            handleStreamFail();
            return;
        }
        emit q->progress(device->pos());
        bytesLeft -= sz;
    }

    void readNextBlockFromTransport()
    {
        quint64 bytesAvail;
        while (bytesLeft && (bytesAvail = connection->bytesAvailable())) {
            quint64 sz = 65536; // shall we respect transport->blockSize() ?
            if (sz > bytesLeft) {
                sz = bytesLeft;
            }
            if (sz > bytesAvail) {
                sz = bytesAvail;
            }
            QByteArray data = connection->read(sz);
            if (data.isEmpty()) {
                handleStreamFail();
                return;
            }
            if (device->write(data) == -1) {
                handleStreamFail();
                return;
            }
            emit q->progress(device->pos());
            bytesLeft -= sz;
        }
        if (!bytesLeft) {
            // TODO send <received>
            setState(State::Finished);
        }
    }
};

Application::Application(const QSharedPointer<Pad> &pad, const QString &contentName, Origin creator, Origin senders) :
    d(new Private)
{
    d->q       = this;
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

void Application::setState(State state)
{
    d->setState(state);
}

Stanza::Error Application::lastError() const
{
    return d->lastError;
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
    //d->state = State::Pending; // basically it's incomming  content. so if we parsed it it's pending. if not parsed if will rejected anyway.
    return d->file.isValid()? Ok: Unparsed;
}

void Application::setFile(const File &file)
{
    d->file = file;
}

File Application::file() const
{
    return d->file;
}

File Application::acceptFile() const
{
    return d->acceptFile;
}

// incoming one? or we have to check real direction
bool Application::setTransport(const QSharedPointer<Transport> &transport)
{
    if (!(transport->features() & Transport::Reliable))
        return false;

    int nsIndex = d->availableTransports.indexOf(transport->pad()->ns());
    if (nsIndex == -1) {
        return false;
    }

    // in case we automatically select a new transport on our own we definitely will come up to this point
    if (d->transport) {
        d->transport->disconnect(this);
        d->transport.reset();
    }

    d->availableTransports.removeAt(nsIndex);
    d->transport = transport;
    connect(transport.data(), &Transport::updated, this, &Application::updated);
    connect(transport.data(), &Transport::connected, this, [this](){
        d->transportReplaceOrigin = Origin::None;
        d->transportReplaceState = State::Finished; // not needed here probably
        d->connection = d->transport->connection();
        connect(d->connection.data(), &Connection::readyRead, this, [this](){
            if (!d->device) {
                return;
            }
            if (d->pad->session()->role() != d->senders) {
                d->readNextBlockFromTransport();
            }
        });
        connect(d->connection.data(), &Connection::bytesWritten, this, [this](qint64 bytes){
            Q_UNUSED(bytes)
            if (d->pad->session()->role() == d->senders && !d->connection->bytesToWrite()) {
                d->writeNextBlockToTransport();
            }
        });
        d->setState(State::Active);
        if (d->acceptFile.range().isValid()) {
            d->bytesLeft = d->acceptFile.range().length;
            emit deviceRequested(d->acceptFile.range().offset, d->bytesLeft);
        } else {
            d->bytesLeft = d->file.size();
            emit deviceRequested(0, d->bytesLeft);
        }
    });

    connect(transport.data(), &Transport::failed, this, [this](){
        d->transportReplaceOrigin = d->pad->session()->role();
        if (d->state >= State::Active) {
            emit updated(); // late failure are unhandled. just notify the remote
            return;
        }
        // we can try to replace the transport
        if (!selectNextTransport()) { // we can do transport-replace here
            if (d->state == State::PrepareLocalOffer && d->creator == d->pad->session()->role()) {
                // we were unable to send even initial offer
                d->setState(State::Finished);
            } else {
                emit updated(); // we have to notify our peer about failure
            }
        } else {
            d->transportReplaceState = State::PrepareLocalOffer;
        }
    });

    if (d->state >= State::Unacked) {
        // seems like we are in transport failure recovery. d->transportFailed may confirm this
        d->transport->prepare();
    }
    return true;
}

Origin Application::transportReplaceOrigin() const
{
    return d->transportReplaceOrigin;
}

bool Application::incomingTransportReplace(const QSharedPointer<Transport> &transport)
{
    auto prev = d->transportReplaceOrigin;
    if (d->pad->session()->role() == Origin::Responder && prev == Origin::Responder && d->transport) {
        // if I'm a responder and tried to send transport-replace too, then push ns back
        d->availableTransports.append(d->transport->pad()->ns());
    }
    d->transportReplaceOrigin = d->pad->session()->peerRole();
    auto ret = setTransport(transport);
    if (ret)
        d->transportReplaceState = State::PrepareLocalOffer;
    else {
        d->transportReplaceOrigin = prev;
        d->lastError.reset();
        // REVIEW We have to fail application here on tie-break or propose another transport
    }

    return ret;
}

QSharedPointer<Transport> Application::transport() const
{
    return d->transport;
}

Action Application::evaluateOutgoingUpdate()
{
    d->updateToSend = Action::NoAction;
    if (!isValid() || d->state == State::Created || d->state == State::Finished) {
        return d->updateToSend;
    }

    auto evaluateTransportReplaceAction = [this]() {
        if (d->transportReplaceState != State::PrepareLocalOffer || !d->transport->isInitialOfferReady())
            return Action::NoAction;

        return d->transportReplaceOrigin == d->pad->session()->role()? Action::TransportReplace : Action::TransportAccept;
    };

    switch (d->state) {
    case State::Created:
        break;
    case State::PrepareLocalOffer:
        if (d->transportReplaceOrigin != Origin::None) {
            if (!d->transport) {
                d->updateToSend = Action::ContentReject; // case me=creator was already handled by this momemnt in case of app.PrepareLocalOffer. see Transport::failed above
            }
            d->updateToSend = evaluateTransportReplaceAction();
            if (d->updateToSend == Action::TransportAccept) {
                d->updateToSend = Action::ContentAccept;
            }
            return d->updateToSend;
        }

        if (d->transport->isInitialOfferReady())
            d->updateToSend = d->creator == d->pad->session()->role()? Action::ContentAdd : Action::ContentAccept;

        break;
    case State::Connecting:
    case State::Pending:
    case State::Active:
        if (d->transportReplaceOrigin != Origin::None) {
            if (d->state == State::Active || !d->transport)
                d->updateToSend = Action::ContentRemove;
            else
                d->updateToSend = evaluateTransportReplaceAction();
            return d->updateToSend;
        }

        if (d->transport->hasUpdates())
            d->updateToSend = Action::TransportInfo;

        break;
    case State::Finishing:
        if (d->transportReplaceOrigin != Origin::None) {
            d->updateToSend = Action::ContentRemove;
        } else {
            d->updateToSend = Action::SessionInfo;
        }
        break;
    default:
        break;
    }
    return d->updateToSend; // TODO
}

OutgoingUpdate Application::takeOutgoingUpdate()
{
    if (d->updateToSend == Action::NoAction) {
        return OutgoingUpdate();
    }

    auto client = d->pad->session()->manager()->client();
    auto doc = client->doc();

    if (d->updateToSend == Action::SessionInfo) {
        if (d->state != State::Finishing) {
            // TODO implement
            return OutgoingUpdate();
        }
        ContentBase cb(d->pad->session()->role(), d->contentName);
        return OutgoingUpdate{QList<QDomElement>() << cb.toXml(doc, "received"), [this](){
                d->setState(State::Finished);
            }
        };
    }

    QDomElement transportEl;
    OutgoingUpdateCB transportCB;

    ContentBase cb(d->creator, d->contentName);
    if (d->state == State::PrepareLocalOffer)
        cb.senders = d->senders;
    QList<QDomElement> updates;
    auto contentEl = cb.toXml(doc, "content");
    updates << contentEl;

    switch (d->updateToSend) {
    case Action::ContentReject:
    case Action::ContentRemove:
        if (d->transportReplaceOrigin != Origin::None)
            updates << Reason(Reason::Condition::FailedTransport).toXml(doc);
        return OutgoingUpdate{updates, [this](){
                d->setState(State::Finished);
            }
        };
    case Action::ContentAdd:
    case Action::ContentAccept:
        Q_ASSERT(d->transport->isInitialOfferReady());
        if (d->file.thumbnail().data.size()) {
            auto thumb = d->file.thumbnail();
            auto bm = client->bobManager();
            BoBData data = bm->append(thumb.data, thumb.mimeType);
            thumb.uri = QLatin1String("cid:") + data.cid();
            d->file.setThumbnail(thumb);
        }
        contentEl.appendChild(doc->createElementNS(NS, QString::fromLatin1("description"))).appendChild(d->file.toXml(doc));
        std::tie(transportEl, transportCB) = d->transport->takeInitialOffer();
        contentEl.appendChild(transportEl);

        d->setState(State::Unacked);
        return OutgoingUpdate{updates, [this, transportCB](){
                if (transportCB) {
                    transportCB();
                }
                d->setState(d->pad->session()->role() == Origin::Initiator? State::Pending : State::Connecting);
            }};
    case Action::TransportInfo:
        Q_ASSERT(d->transport->hasUpdates());
        std::tie(transportEl, transportCB) = d->transport->takeOutgoingUpdate();
        contentEl.appendChild(transportEl);
        return OutgoingUpdate{updates, transportCB};
    case Action::TransportReplace:
    case Action::TransportAccept:
    {
        Q_ASSERT(d->transport->isInitialOfferReady());
        d->transportReplaceState = State::Unacked;
        std::tie(transportEl, transportCB) = d->transport->takeInitialOffer();
        contentEl.appendChild(transportEl);
        auto action = d->updateToSend;
        return OutgoingUpdate{updates, [this,transportCB,action](){
                if (transportCB) {
                    transportCB();
                }
                d->transportReplaceState = action == Action::TransportReplace? State::Pending : State::Finished;
            }};
    }
    default:
        break;
    }

    return OutgoingUpdate(); // TODO
}

bool Application::wantBetterTransport(const QSharedPointer<Transport> &t) const
{
    Q_UNUSED(t)
    return true; // TODO check
}

bool Application::selectNextTransport()
{
    while (d->availableTransports.size()) {
        auto t = d->pad->session()->newOutgoingTransport(d->availableTransports.last());
        if (t && setTransport(t)) {
            return true;
        } else {
            d->availableTransports.removeLast();
        }
    }
    return false;
}

void Application::prepare()
{
    if (!d->transport) {
        selectNextTransport();
    }
    if (d->transport) {
        d->setState(State::PrepareLocalOffer);
        d->transport->prepare();
    }
}

void Application::start()
{
    if (d->transport) {
        d->setState(State::Connecting);
        d->transport->start();
    }
    // TODO we need QIODevice somewhere here
}

bool Application::accept(const QDomElement &el)
{
    File f(el.firstChildElement("file"));
    if (!f.isValid()) {
        return false;
    }
    d->acceptFile = f;
    // TODO validate if accept file matches to the offer
    setState(State::Accepted);
    return true;
}

bool Application::incomingTransportAccept(const QDomElement &transportEl)
{
    if (d->transportReplaceOrigin != d->pad->session()->role()) {
        d->lastError = ErrorUtil::makeOutOfOrder(*d->pad->doc());
        return false;
    }
    if (d->transport->update(transportEl)) {
        d->transportReplaceOrigin = Origin::None;
        d->transportReplaceState = State::Finished;
        if (d->state >= State::Connecting) {
            d->transport->start();
        }
        emit updated();
        return true;
    }
    return false;
}

bool Application::isValid() const
{
    return d->file.isValid() &&  d->contentName.size() > 0 &&
            (d->senders == Origin::Initiator || d->senders == Origin::Responder);
}

void Application::setDevice(QIODevice *dev, bool closeOnFinish)
{
    d->device = dev;
    d->closeDeviceOnFinish = closeOnFinish;
    if (d->senders == d->pad->session()->role()) {
        d->writeNextBlockToTransport();
    } else {
        d->readNextBlockFromTransport();
    }
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
