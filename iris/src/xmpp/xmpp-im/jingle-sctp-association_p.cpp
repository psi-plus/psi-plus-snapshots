/*
 * jignle-sctp-association_p.h - Private parto of Jingle SCTP Association
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

#include "jingle-sctp-association_p.h"
#include "jingle-sctp.h"
#include "jingle-webrtc-datachannel_p.h"

#define SCTP_DEBUG(msg, ...) qDebug("jingle-sctp: " msg, ##__VA_ARGS__)

namespace XMPP { namespace Jingle { namespace SCTP {

    static constexpr int MAX_STREAMS          = 65535; // let's change when we need something but webrtc dc.
    static constexpr int MAX_MESSAGE_SIZE     = 262144;
    static constexpr int MAX_SEND_BUFFER_SIZE = 262144;

    std::weak_ptr<Keeper> Keeper::instance;

    Keeper::Keeper()
    {
        qDebug("init usrsctp");
        DepUsrSCTP::ClassInit();
    }

    Keeper::~Keeper()
    {
        qDebug("deinit usrsctp");
        DepUsrSCTP::ClassDestroy();
    }

    Keeper::Ptr Keeper::use()
    {
        auto i = instance.lock();
        if (!i) {
            i        = std::make_shared<Keeper>();
            instance = i;
        }
        return i;
    }

    AssociationPrivate::AssociationPrivate(Association *q) :
        q(q), keeper(Keeper::use()), assoc(this, MAX_STREAMS, MAX_STREAMS, MAX_MESSAGE_SIZE, MAX_SEND_BUFFER_SIZE, true)
    {
    }

    void AssociationPrivate::OnSctpAssociationConnecting(RTC::SctpAssociation *)
    {
        qDebug("jignle-sctp: on connecting");
    }

    void AssociationPrivate::OnSctpAssociationConnected(RTC::SctpAssociation *)
    {
        qDebug("jignle-sctp: on connected");
        for (auto &channel : channels) {
            channel.staticCast<WebRTCDataChannel>()->connect();
        }
    }

    void AssociationPrivate::OnSctpAssociationFailed(RTC::SctpAssociation *) { qDebug("jignle-sctp: on failed"); }

    void AssociationPrivate::OnSctpAssociationClosed(RTC::SctpAssociation *) { qDebug("jignle-sctp: on closed"); }

    void AssociationPrivate::OnSctpAssociationSendData(RTC::SctpAssociation *, const uint8_t *data, size_t len)
    {
        qDebug("jignle-sctp: on outgoing data");
        QByteArray bytes((char *)data, len);
        QMetaObject::invokeMethod(this, "onOutgoingData", Q_ARG(QByteArray, bytes));
    }

    void AssociationPrivate::OnSctpAssociationMessageReceived(RTC::SctpAssociation *, uint16_t streamId, uint32_t ppid,
                                                              const uint8_t *msg, size_t len)
    {
        qDebug("jignle-sctp: on incoming data");
        QByteArray bytes((char *)msg, len);
        QMetaObject::invokeMethod(this, "onIncomingData", Q_ARG(QByteArray, bytes), Q_ARG(quint16, streamId),
                                  Q_ARG(quint32, ppid));
    }

    void AssociationPrivate::OnSctpAssociationBufferedAmount(RTC::SctpAssociation *sctpAssociation, uint32_t len)
    {
        qDebug("jignle-sctp: on buffered data: %d", len);
        Q_UNUSED(sctpAssociation);
        Q_UNUSED(len);
        if (!dumpingOutogingBuffer)
            procesOutgoingMessageQueue();
        // TODO control buffering to reduce memory consumption
    }

    void AssociationPrivate::OnSctpStreamClosed(RTC::SctpAssociation *sctpAssociation, uint16_t streamId)
    {
        qDebug("jignle-sctp: on stream closed");
        Q_UNUSED(sctpAssociation);
        QMetaObject::invokeMethod(this, "onStreamClosed", Q_ARG(quint16, streamId));
    }

    void AssociationPrivate::handleIncomingDataChannelOpen(const QByteArray &data, quint16 streamId)
    {
        auto channel = WebRTCDataChannel::fromChannelOpen(this, data);

        channel->setStreamId(streamId);
        pendingChannels.append(channel);
        auto it = channels.constFind(streamId);
        if (it != channels.constEnd()) {
            qWarning("datachannel %u was replaced", streamId);
            (*it)->disconnect(this);
            (*it).staticCast<WebRTCDataChannel>()->onDisconnected(WebRTCDataChannel::ChannelReplaced);
        }
        channels.insert(streamId, channel);
        connectChannelSignals(channel);

        // acknowledge channel open instantly
        QByteArray reply(4, 0);
        reply[0] = DCEP_DATA_CHANNEL_ACK;
        write(reply, streamId, PPID_DCEP);

        emit q->newIncomingChannel();
    }

    void AssociationPrivate::setIdSelector(IdSelector selector)
    {
        switch (selector) {
        case IdSelector::Even:
            useOddStreamId = false;
            if (nextStreamId & 1)
                nextStreamId++;
            break;
        case IdSelector::Odd:
            useOddStreamId = true;
            if (!(nextStreamId & 1))
                nextStreamId++;
            break;
        }
    }

    bool AssociationPrivate::write(const QByteArray &data, quint16 streamId, quint32 ppid, Reliability reliable,
                                   bool ordered, quint32 reliability)
    {
        qDebug("jignle-sctp: write %d bytes on stream %u with ppid %u", data.size(), streamId, ppid);
        RTC::DataConsumer consumer;
        consumer.sctpParameters.streamId          = streamId;
        consumer.sctpParameters.ordered           = ordered; // ordered=true also enables reliability
        consumer.sctpParameters.maxPacketLifeTime = reliable == PartialTimers ? reliability : 0;
        consumer.sctpParameters.maxRetransmits    = reliable == PartialRexmit ? reliability : 0;
        bool success;
        assoc.SendSctpMessage(
            &consumer, ppid, reinterpret_cast<const uint8_t *>(data.data()), data.size(),
            new std::function<void(bool)>([this, &success](bool cb_success) { success = cb_success; }));
        return success;
    }

    void AssociationPrivate::procesOutgoingMessageQueue()
    {
        dumpingOutogingBuffer = true;
        // keep going while we can fit the buffer
        while (outgoingMessageQueue.size()) {

            auto const &[connection, message] = outgoingMessageQueue.first();
            if (int(MAX_SEND_BUFFER_SIZE - assoc.GetSctpBufferedAmount()) < message.data.size())
                break;

            bool        ordered  = !(message.channelType & 0x80);
            Reliability reliable = ordered         ? Reliable
                : (message.channelType & 0x3) == 1 ? PartialRexmit
                : (message.channelType & 0x3) == 2 ? PartialTimers
                                                   : Reliable;

            if (write(message.data, message.streamId, PPID_BINARY, reliable, ordered, message.reliability)) {
                int sz = message.data.size();
                connection.staticCast<WebRTCDataChannel>()->onMessageWritten(sz);
            } else if (assoc.isSendBufferFull())
                break;
            else {
                qWarning("unexpected sctp write error");
                connection.staticCast<WebRTCDataChannel>()->onError(QAbstractSocket::SocketResourceError);
            }
            outgoingMessageQueue.removeFirst();
        }
        dumpingOutogingBuffer = false;
    }

    void AssociationPrivate::close(quint16 streamId)
    {
        qDebug("jignle-sctp: close");
        RTC::DataProducer producer;
        producer.sctpParameters.streamId = streamId;
        assoc.DataProducerClosed(&producer);
    }

    quint16 AssociationPrivate::takeNextStreamId()
    {
        if (!channelsLeft)
            return 0xffff; // impossible stream
        auto id = nextStreamId;
        while (channels.contains(id)) {
            id += 2;
            if (id == nextStreamId)
                return 0xffff;
        }
        nextStreamId = id + 2;
        return id;
    }

    Connection::Ptr AssociationPrivate::newChannel(Reliability reliable, bool ordered, quint32 reliability,
                                                   quint16 priority, const QString &label, const QString &protocol)
    {
        SCTP_DEBUG("adding new channel");
        int channelType = int(reliable);
        if (ordered)
            channelType |= 0x80;
        auto channel
            = QSharedPointer<WebRTCDataChannel>::create(this, channelType, priority, reliability, label, protocol);
        if (transportConnected) {
            auto id = takeNextStreamId();
            if (id == 0xffff)
                return {};
            channel->setStreamId(id);
            channels.insert(id, channel);
            channelsLeft--;
            qWarning("TODO negotiate datachannel itself");
        } else {
            pendingLocalChannels.enqueue(channel);
        }
        connectChannelSignals(channel);

        return channel;
    }

    QList<Connection::Ptr> AssociationPrivate::allChannels() const
    {
        QList<Connection::Ptr> ret;
        ret.reserve(channels.size() + pendingLocalChannels.size());
        ret += channels.values();
        ret += pendingLocalChannels;
        return ret;
    }

    Connection::Ptr AssociationPrivate::nextChannel()
    {
        if (pendingChannels.empty())
            return {};
        return pendingChannels.dequeue();
    }

    void AssociationPrivate::onTransportConnected()
    {
        SCTP_DEBUG("starting sctp association");
        transportConnected = true;
        while (pendingLocalChannels.size()) {
            auto channel = pendingLocalChannels.dequeue().staticCast<WebRTCDataChannel>();
            auto id      = takeNextStreamId();
            if (id == 0xffff) { // impossible channel
                channel->onError(QAbstractSocket::SocketResourceError);
            } else {
                channel->setStreamId(id);
                channels.insert(id, channel);
                channelsLeft--;
            }
        }
        assoc.TransportConnected();
    }

    void AssociationPrivate::onTransportError(QAbstractSocket::SocketError error)
    {
        transportConnected = false;
        for (auto &c : channels) {
            c.staticCast<WebRTCDataChannel>()->onError(error);
        }
    }

    void AssociationPrivate::onTransportClosed()
    {
        transportConnected = false;
        for (auto &c : channels) {
            c.staticCast<WebRTCDataChannel>()->onDisconnected(WebRTCDataChannel::TransportClosed);
        }
    }

    void AssociationPrivate::onOutgoingData(const QByteArray &data)
    {
        outgoingPacketsQueue.enqueue(data);
        emit q->readyReadOutgoing();
    }

    void AssociationPrivate::onIncomingData(const QByteArray &data, quint16 streamId, quint32 ppid)
    {
        auto it = channels.find(streamId);
        if (it == channels.end()) {
            if (ppid == PPID_DCEP) {
                if (data.isEmpty()) {
                    qWarning("jingle-sctp: dropping invalid dcep");
                } else if (data[0] == DCEP_DATA_CHANNEL_OPEN) {
                    handleIncomingDataChannelOpen(data, streamId);
                }
            } else
                qWarning("jingle-sctp: data from unknown datachannel. ignoring");
            return;
        }
        it->staticCast<WebRTCDataChannel>()->onIncomingData(data, ppid);
    }

    void AssociationPrivate::onStreamClosed(quint16 streamId)
    {
        auto it = channels.find(streamId);
        if (it == channels.end()) {
            qDebug("jingle-sctp: closing not existing stream %d", streamId);
            return;
        }
        it->staticCast<WebRTCDataChannel>()->onDisconnected(WebRTCDataChannel::ChannelClosed);
    }

    void AssociationPrivate::connectChannelSignals(Connection::Ptr channel)
    {
        auto dc = channel.staticCast<WebRTCDataChannel>();
        dc->setOutgoingCallback([this, weakDc = dc.toWeakRef()](const WebRTCDataChannel::OutgoingDatagram &dg) {
            outgoingMessageQueue.enqueue({ weakDc.lock(), dg });
            procesOutgoingMessageQueue();
        });
    }

}}}
