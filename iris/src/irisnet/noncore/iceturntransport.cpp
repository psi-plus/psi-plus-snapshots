/*
 * Copyright (C) 2010  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "iceturntransport.h"

#include "stunallocate.h"

#include <QtCrypto>

namespace XMPP {
class IceTurnTransport::Private : public QObject {
    Q_OBJECT

public:
    IceTurnTransport *q;
    int               mode;
    TransportAddress  serverAddr;
    QString           relayUser;
    QCA::SecureArray  relayPass;
    TransportAddress  relayAddr;
    TransportAddress  refAddr;
    TurnClient        turn;
    int               turnErrorCode = 0;
    int               debugLevel;
    bool              started = false;

    Private(IceTurnTransport *_q) : QObject(_q), q(_q), turn(this), debugLevel(IceTransport::DL_None)
    {
        connect(&turn, &TurnClient::connected, this, &Private::turn_connected);
        connect(&turn, &TurnClient::tlsHandshaken, this, &Private::turn_tlsHandshaken);
        connect(&turn, &TurnClient::closed, this, &Private::turn_closed);
        connect(&turn, &TurnClient::needAuthParams, this, &Private::turn_needAuthParams);
        connect(&turn, &TurnClient::retrying, this, &Private::turn_retrying);
        connect(&turn, &TurnClient::activated, this, &Private::turn_activated);
        connect(&turn, &TurnClient::readyRead, this, &Private::turn_readyRead);
        connect(&turn, &TurnClient::packetsWritten, this, &Private::turn_packetsWritten);
        connect(&turn, &TurnClient::error, this, &Private::turn_error);
        connect(&turn, &TurnClient::debugLine, this, &Private::turn_debugLine);
    }

    void start()
    {
        turn.setUsername(relayUser);
        turn.setPassword(relayPass);
        turn.connectToHost(serverAddr, (TurnClient::Mode)mode);
    }

    void stop() { turn.close(); }

private slots:
    void turn_connected()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_connected");
    }

    void turn_tlsHandshaken()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_tlsHandshaken");
    }

    void turn_closed()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_closed");

        emit q->stopped();
    }

    void turn_needAuthParams(const TransportAddress &addr)
    {
        // we can get this signal if the user did not provide
        //   creds to us.  however, since this class doesn't support
        //   prompting just continue on as if we had a blank
        //   user/pass
        turn.continueAfterParams(addr);
    }

    void turn_retrying()
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine("turn_retrying");
    }

    void turn_activated()
    {
        StunAllocate *allocate = turn.stunAllocate();

        auto saddr = allocate->reflexiveAddress();
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QLatin1String("Server says we are ") + saddr);
        saddr = allocate->relayedAddress();
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QLatin1String("Server relays via ") + saddr);

        relayAddr = saddr;
        refAddr   = allocate->reflexiveAddress();
        started   = true;

        emit q->started();
    }

    void turn_readyRead() { emit q->readyRead(0); }

    void turn_packetsWritten(int count, const TransportAddress &addr) { emit q->datagramsWritten(0, count, addr); }

    void turn_error(XMPP::TurnClient::Error e)
    {
        if (debugLevel >= IceTransport::DL_Info)
            emit q->debugLine(QString("turn_error: ") + turn.errorString());

        turnErrorCode = e;
        emit q->error(IceTurnTransport::ErrorTurn);
    }

    void turn_debugLine(const QString &line) { emit q->debugLine(line); }
};

IceTurnTransport::IceTurnTransport(QObject *parent) : IceTransport(parent) { d = new Private(this); }

IceTurnTransport::~IceTurnTransport() { delete d; }

void IceTurnTransport::setClientSoftwareNameAndVersion(const QString &str)
{
    d->turn.setClientSoftwareNameAndVersion(str);
}

void IceTurnTransport::setUsername(const QString &user) { d->relayUser = user; }

void IceTurnTransport::setPassword(const QCA::SecureArray &pass) { d->relayPass = pass; }

void IceTurnTransport::setProxy(const TurnClient::Proxy &proxy) { d->turn.setProxy(proxy); }

void IceTurnTransport::start(const TransportAddress &addr, TurnClient::Mode mode)
{
    d->serverAddr = addr;
    d->mode       = mode;
    d->start();
}

const TransportAddress &IceTurnTransport::relayedAddress() const { return d->relayAddr; }

const TransportAddress &IceTurnTransport::reflexiveAddress() const { return d->refAddr; }

bool IceTurnTransport::isStarted() const { return d->started; }

void IceTurnTransport::addChannelPeer(const TransportAddress &addr) { d->turn.addChannelPeer(addr); }

TurnClient::Error IceTurnTransport::turnErrorCode() const { return (TurnClient::Error)d->turnErrorCode; }

void IceTurnTransport::stop() { d->stop(); }

bool IceTurnTransport::hasPendingDatagrams(int path) const
{
    Q_ASSERT(path == 0);
    Q_UNUSED(path)

    return d->turn.packetsToRead() > 0;
}

QByteArray IceTurnTransport::readDatagram(int path, TransportAddress &addr)
{
    Q_ASSERT(path == 0);
    Q_UNUSED(path)

    return d->turn.read(addr);
}

void IceTurnTransport::writeDatagram(int path, const QByteArray &buf, const TransportAddress &addr)
{
    Q_ASSERT(path == 0);
    Q_UNUSED(path)

    d->turn.write(buf, addr);
}

void IceTurnTransport::setDebugLevel(DebugLevel level)
{
    d->debugLevel = level;
    d->turn.setDebugLevel((TurnClient::DebugLevel)level);
}

void IceTurnTransport::changeThread(QThread *thread)
{
    d->turn.changeThread(thread);
    moveToThread(thread);
}

} // namespace XMPP

#include "iceturntransport.moc"
