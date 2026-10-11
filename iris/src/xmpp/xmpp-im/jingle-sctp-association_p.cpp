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

    Keeper::Ptr Keeper::instance;

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
        if (!instance)
            instance = std::make_shared<Keeper>();
        return instance;
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
        if (associationClosed)
            return;
        for (auto &channel : channels) {
            auto dc = channel.staticCast<WebRTCDataChannel>();
            if (!dc->openingDeferred)
                dc->connect();
        }
    }

    void AssociationPrivate::OnSctpAssociationFailed(RTC::SctpAssociation *)
    {
        qWarning("jingle-sctp: association failed");
        QMetaObject::invokeMethod(this, "onAssociationClosed", Qt::QueuedConnection);
    }

    void AssociationPrivate::OnSctpAssociationClosed(RTC::SctpAssociation *)
    {
        qDebug("jingle-sctp: association closed");
        // A channel observer may destroy the association. Notify it only after
        // returning from the usrsctp callback (including its timer callback).
        QMetaObject::invokeMethod(this, "onAssociationClosed", Qt::QueuedConnection);
    }

    void AssociationPrivate::OnSctpAssociationSendData(RTC::SctpAssociation *, const uint8_t *data, size_t len)
    {
        // qDebug("jignle-sctp: on outgoing data");
        QByteArray bytes((char *)data, int(len));
        QMetaObject::invokeMethod(this, "onOutgoingData", Q_ARG(QByteArray, bytes));
    }

    void AssociationPrivate::OnSctpAssociationMessageReceived(RTC::SctpAssociation *, uint16_t streamId, uint32_t ppid,
                                                              const uint8_t *msg, size_t len)
    {
        // qDebug("jignle-sctp: on incoming data");
        QByteArray bytes((char *)msg, int(len));
        QMetaObject::invokeMethod(this, "onIncomingData", Q_ARG(QByteArray, bytes), Q_ARG(quint16, streamId),
                                  Q_ARG(quint32, ppid));
    }

    /**
     * @brief AssociationPrivate::OnSctpAssociationBufferedAmount
     * @param sctpAssociation
     * @param len - number of bytes currently buffered in usrsctp. not more than MAX_SEND_BUFFER_SIZE
     */
    void AssociationPrivate::OnSctpAssociationBufferedAmount(RTC::SctpAssociation *sctpAssociation, size_t len)
    {
        // qDebug("jignle-sctp: on buffered data: %d", len);
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
        QByteArray reply(1, DCEP_DATA_CHANNEL_ACK);
        channel->outgoingCallback({ streamId, 0, PPID_DCEP, 0, reply });

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
        // qDebug("jignle-sctp: write %d bytes on stream %u with ppid %u", data.size(), streamId, ppid);
        RTC::DataConsumer consumer;
        consumer.sctpParameters.streamId          = streamId;
        consumer.sctpParameters.ordered           = ordered; // ordered=true also enables reliability
        consumer.sctpParameters.maxPacketLifeTime = reliable == PartialTimers ? reliability : 0;
        consumer.sctpParameters.maxRetransmits    = reliable == PartialRexmit ? reliability : 0;
        bool success;
        assoc.SendSctpMessage(&consumer, ppid, reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                              new std::function<void(bool)>([&success](bool cb_success) { success = cb_success; }));
        return success;
    }

    void AssociationPrivate::procesOutgoingMessageQueue()
    {
        if (associationClosed || dumpingOutogingBuffer)
            return; // a terminal association cannot accept more writes

        dumpingOutogingBuffer = true;
        // keep going while we can fit the buffer
        while (outgoingMessageQueue.size()) {

            const auto [connection, message] = outgoingMessageQueue.first();
            if (int(MAX_SEND_BUFFER_SIZE - assoc.GetSctpBufferedAmount()) < message.data.size())
                break;

            bool        ordered  = message.ppid == PPID_DCEP || !(message.channelType & 0x80);
            Reliability reliable = ordered         ? Reliable
                : (message.channelType & 0x3) == 1 ? PartialRexmit
                : (message.channelType & 0x3) == 2 ? PartialTimers
                                                   : Reliable;

            if (write(message.data, message.streamId, message.ppid, reliable, ordered, message.reliability)) {
                outgoingMessageQueue.removeFirst();
                int sz = message.data.size();
                if (message.ppid != PPID_DCEP)
                    connection.staticCast<WebRTCDataChannel>()->onMessageWritten(sz);
            } else if (assoc.isSendBufferFull())
                break;
            else {
                outgoingMessageQueue.removeFirst();
                qWarning("unexpected sctp write error");
                connection.staticCast<WebRTCDataChannel>()->onError(QAbstractSocket::SocketResourceError);
            }
        }
        dumpingOutogingBuffer = false;
    }

    void AssociationPrivate::close(quint16 streamId)
    {
        qDebug("jignle-sctp: close");
        // Only bytes already accepted by SCTP belong to its reliable reset
        // tail. Unaccepted writes must not block surviving channels behind a
        // stream that is being reset (or get submitted again after reset).
        discardPendingMessages(streamId);
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
                                                   quint16 priority, const QString &label, const QString &protocol,
                                                   bool deferOpening)
    {
        if (associationClosed)
            return {};
        SCTP_DEBUG("adding new channel");
        int channelType = int(reliable);
        if (!ordered)
            channelType |= 0x80;
        auto channel
            = QSharedPointer<WebRTCDataChannel>::create(this, channelType, reliability, priority, label, protocol);
        channel->openingDeferred = deferOpening;
        if (transportConnected) {
            auto id = takeNextStreamId();
            if (id == 0xffff)
                return {};
            channel->setStreamId(id);
            channels.insert(id, channel);
            channelsLeft--;
        } else {
            pendingLocalChannels.enqueue(channel);
        }
        connectChannelSignals(channel);
        // Channels added after the association handshake need their own DCEP
        // OPEN/ACK exchange. Channels added while SCTP is connecting are opened
        // by OnSctpAssociationConnected together with the initial channels.
        if (!deferOpening && assoc.GetState() == RTC::SctpAssociation::SctpState::CONNECTED)
            channel->connect();

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
        if (associationClosed)
            return;
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
        qWarning("jingle-sctp: transport failed error=%d", int(error));
        closeChannels(WebRTCDataChannel::TransportClosed);
    }

    void AssociationPrivate::onTransportClosed() { closeChannels(WebRTCDataChannel::TransportClosed); }

    void AssociationPrivate::onAssociationClosed() { closeChannels(WebRTCDataChannel::SctpClosed); }

    void AssociationPrivate::closeChannels(WebRTCDataChannel::DisconnectReason reason)
    {
        if (associationClosed)
            return;
        associationClosed  = true;
        transportConnected = false;

        // Retain every channel, including local channels still awaiting DTLS.
        // Detach all ownership and writes before emitting any signal: an
        // observer may destroy this association or close a sibling channel.
        const auto closingChannels = allChannels();
        channels.clear();
        pendingChannels.clear();
        pendingLocalChannels.clear();
        outgoingMessageQueue.clear();
        outgoingPacketsQueue.clear();
        qDebug("jingle-sctp: closing %d channels reason=%d", int(closingChannels.size()), int(reason));
        for (const auto &channel : closingChannels) {
            auto dc              = channel.staticCast<WebRTCDataChannel>();
            dc->association      = nullptr;
            dc->outgoingCallback = {};
            dc->outgoingBufSize  = 0;
        }
        // Use only the retained snapshot after the first notification. Already
        // received data stays readable; connectionClosed waits for its drain.
        for (const auto &channel : closingChannels)
            channel.staticCast<WebRTCDataChannel>()->onDisconnected(reason);
    }

    void AssociationPrivate::onOutgoingData(const QByteArray &data)
    {
        if (associationClosed)
            return;
        outgoingPacketsQueue.enqueue(data);
        emit q->readyReadOutgoing();
    }

    void AssociationPrivate::onIncomingData(const QByteArray &data, quint16 streamId, quint32 ppid)
    {
        if (associationClosed)
            return;
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
        discardPendingMessages(streamId);
        auto it = channels.find(streamId);
        if (it == channels.end()) {
            qDebug("jingle-sctp: closing not existing stream %d", streamId);
            return;
        }

        // Keep a temporary strong reference while dropping association
        // ownership. The application may already have released its own
        // reference by the time the asynchronous SCTP reset completes.
        auto channel = it.value();
        channels.erase(it);
        pendingChannels.removeAll(channel);

        // Only locally allocated parity consumes channelsLeft.
        const bool localStream = bool(streamId & 1) == useOddStreamId;
        if (localStream)
            ++channelsLeft;

        channel.staticCast<WebRTCDataChannel>()->onDisconnected(WebRTCDataChannel::ChannelClosed);
    }

    void AssociationPrivate::discardPendingMessages(quint16 streamId)
    {
        for (auto it = outgoingMessageQueue.begin(); it != outgoingMessageQueue.end();) {
            if (it->second.streamId != streamId) {
                ++it;
                continue;
            }
            if (it->first && it->second.ppid != PPID_DCEP) {
                auto dc = it->first.staticCast<WebRTCDataChannel>();
                dc->outgoingBufSize -= size_t(it->second.data.size());
            }
            it = outgoingMessageQueue.erase(it);
        }
    }

    void AssociationPrivate::connectChannelSignals(Connection::Ptr channel)
    {
        auto dc = channel.staticCast<WebRTCDataChannel>();
        dc->setOutgoingCallback([this, weakDc = dc.toWeakRef()](const WebRTCDataChannel::OutgoingDatagram &dg) {
            if (dg.ppid == PPID_DCEP)
                outgoingMessageQueue.prepend({ weakDc.lock(), dg });
            else
                outgoingMessageQueue.enqueue({ weakDc.lock(), dg });
            procesOutgoingMessageQueue();
        });
    }

}}}
