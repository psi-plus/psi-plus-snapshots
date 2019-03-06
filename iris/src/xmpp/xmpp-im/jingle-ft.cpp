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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "jingle-ft.h"
#include "xmpp_client.h"

namespace XMPP {
namespace Jingle {

namespace FileTransfer {

const QString NS = QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5");

//----------------------------------------------------------------------------
// File
//----------------------------------------------------------------------------
class File::Private : public QSharedData
{
public:
    QDateTime date;
    QString mediaType;
    QString name;
    QString desc;
    QString size;
    Range range;
    Hash hash;
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
    QString mediaType;
    QString name;
    QString desc;
    size_t size;
    Range range{};
    Hash hash;

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

        } else if (ce.tagName() == QLatin1String("desc")) {
            desc = ce.text();

        } else if (ce.tagName() == QLatin1String("hash")) {
            if (ce.namespaceURI() == QLatin1String("urn:xmpp:hashes:2")) {
                hash = Hash(ce);
                if (hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

        } else if (ce.tagName() == QLatin1String("hash-used")) {
            if (ce.namespaceURI() == QLatin1String("urn:xmpp:hashes:2")) {
                hash = Hash(ce);
                if (hash.type() == Hash::Type::Unknown) {
                    return;
                }
            }

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

    d = p;
}

QDomElement File::toXml(QDomDocument *doc) const
{
    Q_UNUSED(doc) // TODO
    return QDomElement(); // TODO
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
Manager::Manager(Client *client):
    XMPP::Jingle::ApplicationManager(client)
{

}

Application* Manager::startApplication(SessionManagerPad *pad, Origin creator, Origin senders)
{
    auto app = new Application(static_cast<Pad*>(pad), creator, senders); // ContentOrigin::Remote
    if (app->isValid()) {
        return app;
    }
    delete app;
    return nullptr;
}

SessionManagerPad *Manager::pad(Session *session)
{
    return new Pad(this, session);
}

void Manager::closeAll()
{

}

//----------------------------------------------------------------------------
// ApplicationManager
//----------------------------------------------------------------------------
class Application::Private
{
public:
    enum State {
        Created,
        SettingTransport,
        Active
    };

    State   state;
    Pad     *pad;
    File    file;
    Origin  creator;
    Origin  senders;
    QSharedPointer<Transport> transport, newTransport;
};

Application::Application(Pad *pad, Origin creator, Origin senders) :
    XMPP::Jingle::Application(pad),
    d(new Private)
{
    d->pad     = pad;
    d->creator = creator;
    d->senders = senders;
}

Application::~Application()
{

}

bool Application::setDescription(const QDomElement &description)
{
    d->file = File(description.firstChildElement("file"));
    return d->file.isValid();
}

void Application::setTransport(const QSharedPointer<Transport> &transport)
{
    d->newTransport = transport;
    d->state = Private::SettingTransport;
}

QSharedPointer<Transport> Application::transport() const
{
    // TODO
    return QSharedPointer<Transport>();
}

Jingle::Action Application::outgoingUpdateType() const
{
    switch (d->state) {
    case Private::Created:
        break;
    case Private::SettingTransport:
        if (d->newTransport->features() & Transport::Reliable) {
            d->transport = d->newTransport; //TODO this won't work. we are const...
            d->newTransport.reset();
        } else {
            // TODO
            // if transport was proposed by other side on session-initiate, we have to generate transport-replace
            // if newTransport is a transport-replace then transport-reject has to be returned
        }
        break;
    case Private::Active:
        return d->transport->outgoingUpdateType();
    }
    return Jingle::NoAction; // TODO
}

bool Application::isReadyForSessionAccept() const
{
    return false; // TODO
}

QDomElement Application::takeOutgoingUpdate()
{
    return QDomElement(); // TODO
}

QDomElement Application::sessionAcceptContent() const
{
    return QDomElement(); // TODO
}

bool Application::isValid() const
{
    return d->file.isValid();
}

Pad::Pad(Manager *manager, Session *session) :
    SessionManagerPad(manager),
    manager(manager),
    session(session)
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

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP
