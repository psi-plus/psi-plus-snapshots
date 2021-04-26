/*
 * jignle-sctp.cpp - Jingle SCTP
 * Copyright (C) 2021  Sergey Ilinykh
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

#include "jingle-sctp.h"

#include "jingle-sctp-association_p.h"
#include "jingle-transport.h"
#include "jingle-webrtc-datachannel_p.h"
#include "xmpp_xmlcommon.h"

#include <QQueue>
#include <QtEndian>

#include <mutex>

#define SCTP_DEBUG(msg, ...) qDebug("jingle-sctp: " msg, ##__VA_ARGS__)

namespace XMPP { namespace Jingle { namespace SCTP {

    QDomElement MapElement::toXml(QDomDocument *doc) const
    {
        QDomElement ret;
        if (protocol == Protocol::None)
            return ret;
        ret = doc->createElementNS(ns(), QLatin1String("sctpmap"));
        ret.setAttribute(QLatin1String("protocol"), QLatin1String("webrtc-datachannel"));
        ret.setAttribute(QLatin1String("number"), port);
        return ret;
    }

    bool MapElement::parse(const QDomElement &el)
    {
        if (el.namespaceURI() != ns()) {
            return false;
        }
        auto p   = el.attribute(QLatin1String("protocol"));
        protocol = (p == QLatin1String("webrtc-datachannel")) ? Protocol::WebRTCDataChannel : Protocol::None;
        port     = el.attribute(QLatin1String("number")).toInt();
        return protocol != Protocol::None && port > 0;
    }

    QDomElement ChannelElement::toXml(QDomDocument *doc) const
    {
        auto el = doc->createElementNS(ns(), QLatin1String("channel"));
        el.setAttribute(QLatin1String("id"), id);
        el.setAttribute(QLatin1String("maxPacketLifeTime"), maxPacketLifeTime);
        el.setAttribute(QLatin1String("maxRetransmits"), maxRetransmits);
        el.setAttribute(QLatin1String("negotiated"), negotiated);
        el.setAttribute(QLatin1String("ordered"), ordered);
        el.setAttribute(QLatin1String("protocol"), protocol);
        return el;
    }

    bool ChannelElement::parse(const QDomElement &el)
    {
        if (el.namespaceURI() != webrtcDcNs()) {
            return false;
        }
        bool ok;
        id = el.attribute(QLatin1String("id")).toUShort(&ok); // REVIEW XEP says id is optional. but is it?
        if (!ok)
            return false;
        QString mplt = el.attribute(QLatin1String("maxPacketLifeTime"));
        QString mrtx = el.attribute(QLatin1String("maxRetransmits"));
        if (!mplt.isEmpty()) {
            maxPacketLifeTime = mplt.toUShort(&ok);
            if (!ok)
                return false;
        }
        if (!mrtx.isEmpty()) {
            maxRetransmits = mrtx.toUShort(&ok);
            if (!ok)
                return false;
        }
        if (maxPacketLifeTime > 0 && maxRetransmits > 0) {
            qWarning("found both maxPacketLifeTime and maxRetransmits. expected just one of them");
            return false;
        }
        XMLHelper::readBoolAttribute(el, QLatin1String("negotiated"), &negotiated);
        XMLHelper::readBoolAttribute(el, QLatin1String("ordered"), &ordered);
        protocol = el.attribute(QLatin1String("protocol"));
        return true;
    }

    QString ns() { return QStringLiteral("urn:xmpp:jingle:transports:dtls-sctp:1"); }
    QString webrtcDcNs() { return QStringLiteral("urn:xmpp:jingle:transports:webrtc-datachannel:0"); }

    Association::Association(QObject *parent) : QObject(parent), d(new AssociationPrivate(this)) { }

    Association::~Association() { }

    void Association::setIdSelector(IdSelector selector) { d->setIdSelector(selector); }

    QByteArray Association::readOutgoing()
    {
        SCTP_DEBUG("read outgoing");
        std::lock_guard<std::mutex> lock(d->mutex);
        return d->outgoingPacketsQueue.isEmpty() ? QByteArray() : d->outgoingPacketsQueue.dequeue();
    }

    void Association::writeIncoming(const QByteArray &data)
    {
        SCTP_DEBUG("write incoming");
        d->assoc.ProcessSctpData(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    }

    int Association::pendingOutgoingDatagrams() const { return d->outgoingPacketsQueue.size(); }

    int Association::pendingChannels() const { return d->pendingChannels.size(); }

    Connection::Ptr Association::nextChannel() { return d->nextChannel(); }

    Connection::Ptr Association::newChannel(Reliability reliable, bool ordered, quint32 reliability, quint16 priority,
                                            const QString &label, const QString &protocol)
    {
        return d->newChannel(reliable, ordered, reliability, priority, label, protocol);
    }

    QList<Connection::Ptr> Association::channels() const { return d->allChannels(); }

    void Association::onTransportConnected() { d->onTransportConnected(); }

    void Association::onTransportError(QAbstractSocket::SocketError error) { d->onTransportError(error); }

    void Association::onTransportClosed() { d->onTransportClosed(); }

}}}
