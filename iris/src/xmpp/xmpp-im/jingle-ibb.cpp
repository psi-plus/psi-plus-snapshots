/*
 * jignle-ibb.cpp - Jingle In-Band Bytestream transport
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

#include "jingle-ibb.h"
#include "xmpp/jid/jid.h"

namespace XMPP {
namespace Jingle {
namespace IBB {

class Connection : public XMPP::Jingle::Connection
{
    Q_OBJECT

    Jid peer;
    QString sid;
    size_t blockSize;
public:
    Connection(const Jid &jid, const QString &sid, size_t blockSize) :
        peer(jid),
        sid(sid),
        blockSize(blockSize)
    {

    }
};

struct Transport::Private
{
    TransportManagerPad::Ptr pad;
    QString sid;
    QSharedPointer<Connection> connection;
    size_t blockSize = 4096;
};

Transport::Transport(const TransportManagerPad::Ptr &pad) :
    d(new Private)
{
    d->pad = pad;
}

Transport::Transport(const TransportManagerPad::Ptr &pad, const QDomElement &transportEl) :
    d(new Private)
{
    d->pad = pad;
    update(transportEl);
    if (d->sid.isEmpty()) {
        d.reset();
        return;
    }
}

Transport::~Transport()
{

}

TransportManagerPad::Ptr Transport::pad() const
{
    return d->pad;
}

void Transport::prepare()
{
    emit updated();
}

void Transport::start()
{
    d->connection.reset(new Connection(d->pad->session()->peer(), d->sid, d->blockSize));
}

bool Transport::update(const QDomElement &transportEl)
{
    auto bs = transportEl.attribute(QString::fromLatin1("block-size"));
    if (!bs.isEmpty()) {
        size_t bsn = bs.toULongLong();
        if (bsn && bsn <= d->blockSize) {
            d->blockSize = bsn;
        }
    }
}

bool Transport::hasUpdates() const
{

}

OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate()
{

}

bool Transport::isValid() const
{
    return true;
}

Transport::Features Transport::features() const
{
    return AlwaysConnect | Reliable | Slow;
}

QString Transport::sid() const
{
    return d->sid;
}

Connection::Ptr Transport::connection() const
{
    return d->connection.staticCast<XMPP::Jingle::Connection>();
}

size_t Transport::blockSize() const
{
    return d->blockSize;
}

Pad::Pad(Manager *manager, Session *session)
{

}

QString Pad::ns() const
{
    return NS;
}

Session *Pad::session() const
{
    return nullptr;
}

TransportManager *Pad::manager() const
{
    return nullptr;
}

QString Pad::generateSid() const
{
    return QString();
}

void Pad::registerSid(const QString &sid)
{

}

} // namespace IBB
} // namespace Jingle
} // namespace XMPP
