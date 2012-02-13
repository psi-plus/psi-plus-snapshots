/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "stunallocate.h"

#include <QMetaType>
#include <QHostAddress>
#include <QTimer>
#include <QtCrypto>
#include "objectsession.h"
#include "stunutil.h"
#include "stunmessage.h"
#include "stuntypes.h"
#include "stuntransaction.h"

// permissions last 5 minutes, update them every 4 minutes
#define PERM_INTERVAL  (4 * 60 * 1000)

// channels last 10 minutes, update them every 9 minutes
#define CHAN_INTERVAL  (9 * 60 * 1000)

namespace XMPP {

void releaseAndDeleteLater(QObject *owner, QObject *obj)
{
	obj->disconnect(owner);
	obj->setParent(0);
	obj->deleteLater();
}

// return size of channelData packet, or -1
static int check_channelData(const quint8 *data, int size)
{
	// top two bits are never zero for ChannelData
	if((data[0] & 0xc0) == 0)
		return -1;

	if(size < 4)
		return -1;

	quint16 len = StunUtil::read16(data + 2);
	if(size - 4 < (int)len)
		return -1;

	// data from a stream must be 4 byte aligned
	int plen = len;
	int remainder = plen % 4;
	if(remainder != 0)
		plen += (4 - remainder);

	int need = plen + 4;
	if(size < need)
		return -1;

	return need;
}

class StunAllocatePermission : public QObject
{
	Q_OBJECT

public:
	QTimer *timer;
	StunTransactionPool *pool;
	StunTransaction *trans;
	QHostAddress stunAddr;
	int stunPort;
	QHostAddress addr;
	bool active;

	enum Error
	{
		ErrorGeneric,
		ErrorProtocol,
		ErrorCapacity,
		ErrorForbidden,
		ErrorRejected,
		ErrorTimeout
	};

	StunAllocatePermission(StunTransactionPool *_pool, const QHostAddress &_addr) :
		QObject(_pool),
		pool(_pool),
		trans(0),
		addr(_addr),
		active(false)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
		timer->setInterval(PERM_INTERVAL);
	}

	~StunAllocatePermission()
	{
		cleanup();

		releaseAndDeleteLater(this, timer);
	}

	void start(const QHostAddress &_addr, int _port)
	{
		Q_ASSERT(!active);

		stunAddr = _addr;
		stunPort = _port;

		doTransaction();
	}

	static StunAllocate::Error errorToStunAllocateError(Error e)
	{
		switch(e)
		{
			case ErrorProtocol:
				return StunAllocate::ErrorProtocol;
			case ErrorCapacity:
				return StunAllocate::ErrorCapacity;
			case ErrorForbidden:
			case ErrorRejected:
				return StunAllocate::ErrorRejected;
			case ErrorTimeout:
				return StunAllocate::ErrorTimeout;
			default:
				return StunAllocate::ErrorGeneric;
		}
	}

signals:
	void ready();
	void error(XMPP::StunAllocatePermission::Error e, const QString &reason);

private:
	void cleanup()
	{
		delete trans;
		trans = 0;

		timer->stop();

		active = false;
	}

	void doTransaction()
	{
		Q_ASSERT(!trans);
		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));
		trans->start(pool, stunAddr, stunPort);
	}

	void restartTimer()
	{
		timer->start();
	}

private slots:
	void trans_createMessage(const QByteArray &transactionId)
	{
		// CreatePermission
		StunMessage message;
		message.setMethod(StunTypes::CreatePermission);
		message.setId((const quint8 *)transactionId.data());

		QList<StunMessage::Attribute> list;

		// we only do one address per permission request, because
		//   otherwise if we receive an error it would be ambiguous
		//   as to which address the error applies to

		{
			StunMessage::Attribute a;
			a.type = StunTypes::XOR_PEER_ADDRESS;
			a.value = StunTypes::createXorPeerAddress(addr, 0, message.magic(), message.id());
			list += a;
		}

		message.setAttributes(list);

		trans->setMessage(message);
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		delete trans;
		trans = 0;

		bool err = false;
		int code;
		QString reason;
		if(response.mclass() == StunMessage::ErrorResponse)
		{
			if(!StunTypes::parseErrorCode(response.attribute(StunTypes::ERROR_CODE), &code, &reason))
			{
				cleanup();
				emit error(ErrorProtocol, "Unable to parse ERROR-CODE in error response.");
				return;
			}

			err = true;
		}

		if(err)
		{
			cleanup();

			if(code == StunTypes::InsufficientCapacity)
				emit error(ErrorCapacity, reason);
			else if(code == StunTypes::Forbidden)
				emit error(ErrorForbidden, reason);
			else
				emit error(ErrorRejected, reason);

			return;
		}

		restartTimer();

		if(!active)
		{
			active = true;
			emit ready();
		}
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		cleanup();

		if(e == XMPP::StunTransaction::ErrorTimeout)
			emit error(ErrorTimeout, "Request timed out.");
		else
			emit error(ErrorGeneric, "Generic transaction error.");
	}

	void timer_timeout()
	{
		doTransaction();
	}
};

class StunAllocateChannel : public QObject
{
	Q_OBJECT

public:
	QTimer *timer;
	StunTransactionPool *pool;
	StunTransaction *trans;
	QHostAddress stunAddr;
	int stunPort;
	int channelId;
	QHostAddress addr;
	int port;
	bool active;

	enum Error
	{
		ErrorGeneric,
		ErrorProtocol,
		ErrorCapacity,
		ErrorForbidden,
		ErrorRejected,
		ErrorTimeout
	};

	StunAllocateChannel(StunTransactionPool *_pool, int _channelId, const QHostAddress &_addr, int _port) :
		QObject(_pool),
		pool(_pool),
		trans(0),
		channelId(_channelId),
		addr(_addr),
		port(_port),
		active(false)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
		timer->setInterval(CHAN_INTERVAL);
	}

	~StunAllocateChannel()
	{
		cleanup();

		releaseAndDeleteLater(this, timer);
	}

	void start(const QHostAddress &_addr, int _port)
	{
		Q_ASSERT(!active);

		stunAddr = _addr;
		stunPort = _port;

		doTransaction();
	}

	static StunAllocate::Error errorToStunAllocateError(Error e)
	{
		switch(e)
		{
			case ErrorProtocol:
				return StunAllocate::ErrorProtocol;
			case ErrorCapacity:
				return StunAllocate::ErrorCapacity;
			case ErrorForbidden:
			case ErrorRejected:
				return StunAllocate::ErrorRejected;
			case ErrorTimeout:
				return StunAllocate::ErrorTimeout;
			default:
				return StunAllocate::ErrorGeneric;
		}
	}

signals:
	void ready();
	void error(XMPP::StunAllocateChannel::Error e, const QString &reason);

private:
	void cleanup()
	{
		delete trans;
		trans = 0;

		timer->stop();

		channelId = -1;
		active = false;
	}

	void doTransaction()
	{
		Q_ASSERT(!trans);
		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));
		trans->start(pool, stunAddr, stunPort);
	}

	void restartTimer()
	{
		timer->start();
	}

private slots:
	void trans_createMessage(const QByteArray &transactionId)
	{
		// ChannelBind
		StunMessage message;
		message.setMethod(StunTypes::ChannelBind);
		message.setId((const quint8 *)transactionId.data());

		QList<StunMessage::Attribute> list;

		{
			StunMessage::Attribute a;
			a.type = StunTypes::CHANNEL_NUMBER;
			a.value = StunTypes::createChannelNumber(channelId);
			list += a;
		}

		{
			StunMessage::Attribute a;
			a.type = StunTypes::XOR_PEER_ADDRESS;
			a.value = StunTypes::createXorPeerAddress(addr, port, message.magic(), message.id());
			list += a;
		}

		message.setAttributes(list);

		trans->setMessage(message);
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		delete trans;
		trans = 0;

		bool err = false;
		int code;
		QString reason;
		if(response.mclass() == StunMessage::ErrorResponse)
		{
			if(!StunTypes::parseErrorCode(response.attribute(StunTypes::ERROR_CODE), &code, &reason))
			{
				cleanup();
				emit error(ErrorProtocol, "Unable to parse ERROR-CODE in error response.");
				return;
			}

			err = true;
		}

		if(err)
		{
			cleanup();

			if(code == StunTypes::InsufficientCapacity)
				emit error(ErrorCapacity, reason);
			else if(code == StunTypes::Forbidden)
				emit error(ErrorForbidden, reason);
			else
				emit error(ErrorRejected, reason);

			return;
		}

		restartTimer();

		if(!active)
		{
			active = true;
			emit ready();
		}
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		cleanup();

		if(e == XMPP::StunTransaction::ErrorTimeout)
			emit error(ErrorTimeout, "Request timed out.");
		else
			emit error(ErrorGeneric, "Generic transaction error.");
	}

	void timer_timeout()
	{
		doTransaction();
	}
};

class StunAllocate::Private : public QObject
{
	Q_OBJECT

public:
	enum DontFragmentState
	{
		DF_Unknown,
		DF_Supported,
		DF_Unsupported
	};

	enum State
	{
		Stopped,
		Starting,
		Started,
		Refreshing,
		Stopping,
		Erroring // like stopping, but emits error when finished
	};

	StunAllocate *q;
	ObjectSession sess;
	StunTransactionPool *pool;
	StunTransaction *trans;
	QHostAddress stunAddr;
	int stunPort;
	State state;
	QString errorString;
	DontFragmentState dfState;
	QString clientSoftware, serverSoftware;
	QHostAddress reflexiveAddress, relayedAddress;
	int reflexivePort, relayedPort;
	StunMessage msg;
	int allocateLifetime;
	QTimer *allocateRefreshTimer;
	QList<StunAllocatePermission*> perms;
	QList<StunAllocateChannel*> channels;
	QList<QHostAddress> permsOut;
	QList<StunAllocate::Channel> channelsOut;
	int erroringCode;
	QString erroringString;

	Private(StunAllocate *_q) :
		QObject(_q),
		q(_q),
		sess(this),
		pool(0),
		trans(0),
		state(Stopped),
		dfState(DF_Unknown),
		erroringCode(-1)
	{
		allocateRefreshTimer = new QTimer(this);
		connect(allocateRefreshTimer, SIGNAL(timeout()), SLOT(refresh()));
		allocateRefreshTimer->setSingleShot(true);
	}

	~Private()
	{
		cleanup();

		releaseAndDeleteLater(this, allocateRefreshTimer);
	}

	void start(const QHostAddress &_addr = QHostAddress(), int _port = -1)
	{
		Q_ASSERT(state == Stopped);

		stunAddr = _addr;
		stunPort = _port;

		state = Starting;
		doTransaction();
	}

	void stop()
	{
		// erroring already?  no need to do anything
		if(state == Erroring)
			return;

		Q_ASSERT(state == Started);

		cleanupTasks();
		state = Stopping;
		doTransaction();
	}

	void stopWithError(int code, const QString &str)
	{
		Q_ASSERT(state == Started);

		cleanupTasks();
		erroringCode = code;
		erroringString = str;
		state = Erroring;
		doTransaction();
	}

	void setPermissions(const QList<QHostAddress> &newPerms)
	{
		// if currently erroring out, skip
		if(state == Erroring)
			return;

		Q_ASSERT(state == Started);

		int freeCount = 0;

		// removed?
		for(int n = 0; n < perms.count(); ++n)
		{
			bool found = false;
			for(int k = 0; k < newPerms.count(); ++k)
			{
				if(newPerms[k] == perms[n]->addr)
				{
					found = true;
					break;
				}
			}

			if(!found)
			{
				// delete related channels
				for(int j = 0; j < channels.count(); ++j)
				{
					if(channels[j]->addr == perms[n]->addr)
					{
						delete channels[j];
						channels.removeAt(j);
						--j; // adjust position
					}
				}

				++freeCount;

				delete perms[n];
				perms.removeAt(n);
				--n; // adjust position
			}
		}

		if(freeCount > 0)
		{
			// removals count as a change, so emit the signal
			sess.deferExclusive(q, "permissionsChanged");

			// wake up inactive perms now that we've freed space
			for(int n = 0; n < perms.count(); ++n)
			{
				if(!perms[n]->active)
					perms[n]->start(stunAddr, stunPort);
			}
		}

		// added?
		for(int n = 0; n < newPerms.count(); ++n)
		{
			bool found = false;
			for(int k = 0; k < perms.count(); ++k)
			{
				if(perms[k]->addr == newPerms[n])
				{
					found = true;
					break;
				}
			}

			if(!found)
			{
				StunAllocatePermission *perm = new StunAllocatePermission(pool, newPerms[n]);
				connect(perm, SIGNAL(ready()), SLOT(perm_ready()));
				connect(perm, SIGNAL(error(XMPP::StunAllocatePermission::Error, const QString &)), SLOT(perm_error(XMPP::StunAllocatePermission::Error, const QString &)));
				perms += perm;
				perm->start(stunAddr, stunPort);
			}
		}
	}

	void setChannels(const QList<StunAllocate::Channel> &newChannels)
	{
		// if currently erroring out, skip
		if(state == Erroring)
			return;

		Q_ASSERT(state == Started);

		int freeCount = 0;

		// removed?
		for(int n = 0; n < channels.count(); ++n)
		{
			bool found = false;
			for(int k = 0; k < newChannels.count(); ++k)
			{
				if(newChannels[k].address == channels[n]->addr && newChannels[k].port == channels[n]->port)
				{
					found = true;
					break;
				}
			}

			if(!found)
			{
				++freeCount;

				delete channels[n];
				channels.removeAt(n);
				--n; // adjust position
			}
		}

		if(freeCount > 0)
		{
			// removals count as a change, so emit the signal
			sess.deferExclusive(q, "channelsChanged");

			// wake up inactive channels now that we've freed space
			for(int n = 0; n < channels.count(); ++n)
			{
				if(!channels[n]->active)
				{
					int channelId = getFreeChannelNumber();

					// out of channels?  give up
					if(channelId == -1)
						break;

					channels[n]->channelId = channelId;
					channels[n]->start(stunAddr, stunPort);
				}
			}
		}

		// added?
		for(int n = 0; n < newChannels.count(); ++n)
		{
			bool found = false;
			for(int k = 0; k < channels.count(); ++k)
			{
				if(channels[k]->addr == newChannels[n].address && channels[k]->port == newChannels[n].port)
				{
					found = true;
					break;
				}
			}

			if(!found)
			{
				// look up the permission for this channel
				int at = -1;
				for(int k = 0; k < perms.count(); ++k)
				{
					if(perms[k]->addr == newChannels[n].address)
					{
						at = k;
						break;
					}
				}

				// only install a channel if we have a permission
				if(at != -1)
				{
					int channelId = getFreeChannelNumber();

					StunAllocateChannel *channel = new StunAllocateChannel(pool, channelId, newChannels[n].address, newChannels[n].port);
					connect(channel, SIGNAL(ready()), SLOT(channel_ready()));
					connect(channel, SIGNAL(error(XMPP::StunAllocateChannel::Error, const QString &)), SLOT(channel_error(XMPP::StunAllocateChannel::Error, const QString &)));
					channels += channel;

					if(channelId != -1)
						channel->start(stunAddr, stunPort);
				}
			}
		}
	}

	int getFreeChannelNumber()
	{
		for(int tryId = 0x4000; tryId <= 0x7fff; ++tryId)
		{
			bool found = false;
			for(int n = 0; n < channels.count(); ++n)
			{
				if(channels[n]->channelId == tryId)
				{
					found = true;
					break;
				}
			}

			if(!found)
				return tryId;
		}

		return -1;
	}

	int getChannel(const QHostAddress &addr, int port)
	{
		for(int n = 0; n < channels.count(); ++n)
		{
			if(channels[n]->active && channels[n]->addr == addr && channels[n]->port == port)
				return channels[n]->channelId;
		}

		return -1;
	}

	// note that this function works even for inactive channels, so that
	//   incoming traffic that is received out-of-order with a
	//   ChannelBind success response is still processable
	bool getAddressPort(int channelId, QHostAddress *addr, int *port)
	{
		for(int n = 0; n < channels.count(); ++n)
		{
			if(channels[n]->channelId == channelId)
			{
				*addr = channels[n]->addr;
				*port = channels[n]->port;
				return true;
			}
		}

		return false;
	}

private:
	void cleanup()
	{
		sess.reset();

		cleanupTasks();

		erroringCode = -1;
		erroringString.clear();

		state = Stopped;
	}

	// stop refreshing, permissions, and channelbinds
	void cleanupTasks()
	{
		delete trans;
		trans = 0;

		allocateRefreshTimer->stop();

		qDeleteAll(channels);
		channels.clear();
		channelsOut.clear();

		qDeleteAll(perms);
		perms.clear();
		permsOut.clear();
	}

	void doTransaction()
	{
		Q_ASSERT(!trans);
		trans = new StunTransaction(this);
		connect(trans, SIGNAL(createMessage(const QByteArray &)), SLOT(trans_createMessage(const QByteArray &)));
		connect(trans, SIGNAL(finished(const XMPP::StunMessage &)), SLOT(trans_finished(const XMPP::StunMessage &)));
		connect(trans, SIGNAL(error(XMPP::StunTransaction::Error)), SLOT(trans_error(XMPP::StunTransaction::Error)));
		trans->start(pool, stunAddr, stunPort);
	}

	void restartRefreshTimer()
	{
		// refresh 1 minute shy of the lifetime
		allocateRefreshTimer->start((allocateLifetime - 60) * 1000);
	}

	bool updatePermsOut()
	{
		QList<QHostAddress> newList;

		for(int n = 0; n < perms.count(); ++n)
		{
			if(perms[n]->active)
				newList += perms[n]->addr;
		}

		if(newList == permsOut)
			return false;

		permsOut = newList;
		return true;
	}

	bool updateChannelsOut()
	{
		QList<StunAllocate::Channel> newList;

		for(int n = 0; n < channels.count(); ++n)
		{
			if(channels[n]->active)
				newList += StunAllocate::Channel(channels[n]->addr, channels[n]->port);
		}

		if(newList == channelsOut)
			return false;

		channelsOut = newList;
		return true;
	}

private slots:
	void refresh()
	{
		Q_ASSERT(state == Started);

		state = Refreshing;
		doTransaction();
	}

	void trans_createMessage(const QByteArray &transactionId)
	{
		if(state == Starting)
		{
			// send Allocate request
			StunMessage message;
			message.setMethod(StunTypes::Allocate);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			if(!clientSoftware.isEmpty())
			{
				StunMessage::Attribute a;
				a.type = StunTypes::SOFTWARE;
				a.value = StunTypes::createSoftware(clientSoftware);
				list += a;
			}

			{
				StunMessage::Attribute a;
				a.type = StunTypes::LIFETIME;
				a.value = StunTypes::createLifetime(3600);
				list += a;
			}

			{
				StunMessage::Attribute a;
				a.type = StunTypes::REQUESTED_TRANSPORT;
				a.value = StunTypes::createRequestedTransport(17); // 17=UDP
				list += a;
			}

			if(dfState == DF_Unknown)
			{
				StunMessage::Attribute a;
				a.type = StunTypes::DONT_FRAGMENT;
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
		else if(state == Stopping || state == Erroring)
		{
			StunMessage message;
			message.setMethod(StunTypes::Refresh);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			{
				StunMessage::Attribute a;
				a.type = StunTypes::LIFETIME;
				a.value = StunTypes::createLifetime(0);
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
		else if(state == Refreshing)
		{
			StunMessage message;
			message.setMethod(StunTypes::Refresh);
			message.setId((const quint8 *)transactionId.data());

			QList<StunMessage::Attribute> list;

			{
				StunMessage::Attribute a;
				a.type = StunTypes::LIFETIME;
				a.value = StunTypes::createLifetime(3600);
				list += a;
			}

			message.setAttributes(list);

			trans->setMessage(message);
		}
	}

	void trans_finished(const XMPP::StunMessage &response)
	{
		delete trans;
		trans = 0;

		bool error = false;
		int code;
		QString reason;
		if(response.mclass() == StunMessage::ErrorResponse)
		{
			if(!StunTypes::parseErrorCode(response.attribute(StunTypes::ERROR_CODE), &code, &reason))
			{
				cleanup();
				errorString = "Unable to parse ERROR-CODE in error response.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			error = true;
		}

		if(state == Starting)
		{
			if(error)
			{
				if(code == StunTypes::UnknownAttribute)
				{
					QList<quint16> typeList;
					if(!StunTypes::parseUnknownAttributes(response.attribute(StunTypes::UNKNOWN_ATTRIBUTES), &typeList))
					{
						cleanup();
						errorString = "Unable to parse UNKNOWN-ATTRIBUTES in 420 (Unknown Attribute) error response.";
						emit q->error(StunAllocate::ErrorProtocol);
						return;
					}

					if(typeList.contains(StunTypes::DONT_FRAGMENT))
					{
						dfState = DF_Unsupported;

						// stay in same state, try again
						doTransaction();
					}
					else
					{
						cleanup();
						errorString = reason;
						emit q->error(StunAllocate::ErrorGeneric);
					}

					return;
				}
				else if(code == StunTypes::AllocationMismatch)
				{
					cleanup();
					errorString = "437 (Allocation Mismatch).";
					emit q->error(StunAllocate::ErrorMismatch);
					return;
				}
				else if(code == StunTypes::InsufficientCapacity)
				{
					cleanup();
					errorString = reason;
					emit q->error(StunAllocate::ErrorCapacity);
					return;
				}
				else if(code == StunTypes::Unauthorized)
				{
					cleanup();
					errorString = "Unauthorized";
					emit q->error(StunAllocate::ErrorAuth);
					return;
				}
				else
				{
					cleanup();
					errorString = reason;
					emit q->error(StunAllocate::ErrorGeneric);
					return;
				}
			}

			quint32 lifetime;
			if(!StunTypes::parseLifetime(response.attribute(StunTypes::LIFETIME), &lifetime))
			{
				cleanup();
				errorString = "Unable to parse LIFETIME.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			QHostAddress raddr;
			quint16 rport;
			if(!StunTypes::parseXorRelayedAddress(response.attribute(StunTypes::XOR_RELAYED_ADDRESS), response.magic(), response.id(), &raddr, &rport))
			{
				cleanup();
				errorString = "Unable to parse XOR-RELAYED-ADDRESS.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			QHostAddress saddr;
			quint16 sport;
			if(!StunTypes::parseXorMappedAddress(response.attribute(StunTypes::XOR_MAPPED_ADDRESS), response.magic(), response.id(), &saddr, &sport))
			{
				cleanup();
				errorString = "Unable to parse XOR-MAPPED-ADDRESS.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			if(lifetime < 120)
			{
				state = Started; // stopWithError requires this
				stopWithError(StunAllocate::ErrorProtocol,
					"LIFETIME is less than two minutes.  That is ridiculous.");
				return;
			}

			QString str;
			if(StunTypes::parseSoftware(response.attribute(StunTypes::SOFTWARE), &str))
			{
				serverSoftware = str;
			}

			allocateLifetime = lifetime;
			relayedAddress = raddr;
			relayedPort = rport;
			reflexiveAddress = saddr;
			reflexivePort = sport;

			if(dfState == DF_Unknown)
				dfState = DF_Supported;

			state = Started;
			restartRefreshTimer();

			emit q->started();
		}
		else if(state == Stopping || state == Erroring)
		{
			if(error)
			{
				// AllocationMismatch on session cancel doesn't count as an error
				if(code != StunTypes::AllocationMismatch)
				{
					cleanup();
					errorString = reason;
					emit q->error(StunAllocate::ErrorGeneric);
					return;
				}
			}

			if(state == Stopping)
			{
				// cleanup will set the state to Stopped
				cleanup();
				emit q->stopped();
			}
			else // Erroring
			{
				int code = erroringCode;
				QString str = erroringString;

				// cleanup will set the state to Stopped
				cleanup();
				errorString = str;
				emit q->error((StunAllocate::Error)code);
			}
		}
		else if(state == Refreshing)
		{
			if(error)
			{
				cleanup();
				errorString = reason;
				emit q->error(StunAllocate::ErrorRejected);
				return;
			}

			quint32 lifetime;
			if(!StunTypes::parseLifetime(response.attribute(StunTypes::LIFETIME), &lifetime))
			{
				cleanup();
				errorString = "Unable to parse LIFETIME.";
				emit q->error(StunAllocate::ErrorProtocol);
				return;
			}

			allocateLifetime = lifetime;

			state = Started;
			restartRefreshTimer();
		}
	}

	void perm_ready()
	{
		if(updatePermsOut())
			emit q->permissionsChanged();
	}

	void perm_error(XMPP::StunAllocatePermission::Error e, const QString &reason)
	{
		if(e == StunAllocatePermission::ErrorCapacity)
		{
			// if we aren't allowed to make anymore permissions,
			//   don't consider this an error.  the perm stays
			//   in the list inactive.  we'll try it again if
			//   any perms get removed.
			return;
		}
		else if(e == StunAllocatePermission::ErrorForbidden)
		{
			// silently discard the permission request
			StunAllocatePermission *perm = (StunAllocatePermission *)sender();
			QHostAddress addr = perm->addr;
			delete perm;
			perms.removeAll(perm);
			emit q->debugLine(QString("Warning: permission forbidden to %1").arg(addr.toString()));
			return;
		}

		cleanup();
		errorString = reason;
		emit q->error(StunAllocatePermission::errorToStunAllocateError(e));
	}

	void channel_ready()
	{
		if(updateChannelsOut())
			emit q->channelsChanged();
	}

	void channel_error(XMPP::StunAllocateChannel::Error e, const QString &reason)
	{
		if(e == StunAllocateChannel::ErrorCapacity)
		{
			// if we aren't allowed to make anymore channels,
			//   don't consider this an error.  the channel stays
			//   in the list inactive.  we'll try it again if
			//   any channels get removed.
			return;
		}

		cleanup();
		errorString = reason;
		emit q->error(StunAllocateChannel::errorToStunAllocateError(e));
	}

	void trans_error(XMPP::StunTransaction::Error e)
	{
		delete trans;
		trans = 0;

		cleanup();

		if(e == StunTransaction::ErrorTimeout)
		{
			errorString = "Request timed out.";
			emit q->error(StunAllocate::ErrorTimeout);
		}
		else
		{
			errorString = "Generic transaction error.";
			emit q->error(StunAllocate::ErrorGeneric);
		}
	}
};

StunAllocate::StunAllocate(StunTransactionPool *pool) :
	QObject(pool)
{
	d = new Private(this);
	d->pool = pool;
}

StunAllocate::~StunAllocate()
{
	delete d;
}

void StunAllocate::setClientSoftwareNameAndVersion(const QString &str)
{
	d->clientSoftware = str;
}

void StunAllocate::start()
{
	d->start();
}

void StunAllocate::start(const QHostAddress &addr, int port)
{
	d->start(addr, port);
}

void StunAllocate::stop()
{
	d->stop();
}

QString StunAllocate::serverSoftwareNameAndVersion() const
{
	return d->serverSoftware;
}

QHostAddress StunAllocate::reflexiveAddress() const
{
	return d->reflexiveAddress;
}

int StunAllocate::reflexivePort() const
{
	return d->reflexivePort;
}

QHostAddress StunAllocate::relayedAddress() const
{
	return d->relayedAddress;
}

int StunAllocate::relayedPort() const
{
	return d->relayedPort;
}

QList<QHostAddress> StunAllocate::permissions() const
{
	return d->permsOut;
}

void StunAllocate::setPermissions(const QList<QHostAddress> &perms)
{
	d->setPermissions(perms);
}

QList<StunAllocate::Channel> StunAllocate::channels() const
{
	return d->channelsOut;
}

void StunAllocate::setChannels(const QList<Channel> &channels)
{
	d->setChannels(channels);
}

int StunAllocate::packetHeaderOverhead(const QHostAddress &addr, int port) const
{
	int channelId = d->getChannel(addr, port);

	if(channelId != -1)
	{
		// overhead of ChannelData
		if(d->pool->mode() == StunTransaction::Udp)
			return 4;
		else // Tcp
			return 4 + 3; // add 3 for potential padding
	}
	else
	{
		// we add 3 for potential padding
		if(d->dfState == StunAllocate::Private::DF_Supported)
		{
			// overhead of STUN-based data, with DONT_FRAGMENT
			return 40 + 3;
		}
		else
		{
			// overhead of STUN-based data, without DONT-FRAGMENT
			return 36 + 3;
		}
	}

	return -1;
}

QByteArray StunAllocate::encode(const QByteArray &datagram, const QHostAddress &addr, int port)
{
	int channelId = d->getChannel(addr, port);

	if(channelId != -1)
	{
		if(datagram.size() > 65535)
			return QByteArray();

		quint16 num = channelId;
		quint16 len = datagram.size();

		int plen = len;

		// in tcp mode, round to up to nearest 4 bytes
		if(d->pool->mode() == StunTransaction::Tcp)
		{
			int remainder = plen % 4;
			if(remainder != 0)
				plen += (4 - remainder);
		}

		QByteArray out(4 + plen, 0);
		StunUtil::write16((quint8 *)out.data(), num);
		StunUtil::write16((quint8 *)out.data() + 2, len);
		memcpy(out.data() + 4, datagram.data(), datagram.size());

		return out;
	}
	else
	{
		StunMessage message;
		message.setClass(StunMessage::Indication);
		message.setMethod(StunTypes::Send);
		QByteArray id = d->pool->generateId();
		message.setId((const quint8 *)id.data());

		QList<StunMessage::Attribute> list;

		{
			StunMessage::Attribute a;
			a.type = StunTypes::XOR_PEER_ADDRESS;
			a.value = StunTypes::createXorPeerAddress(addr, port, message.magic(), message.id());
			list += a;
		}

		if(d->dfState == StunAllocate::Private::DF_Supported)
		{
			StunMessage::Attribute a;
			a.type = StunTypes::DONT_FRAGMENT;
			list += a;
		}

		{
			StunMessage::Attribute a;
			a.type = StunTypes::DATA;
			a.value = datagram;
			list += a;
		}

		message.setAttributes(list);

		return message.toBinary();
	}
}

QByteArray StunAllocate::decode(const QByteArray &encoded, QHostAddress *addr, int *port)
{
	if(encoded.size() < 4)
		return QByteArray();

	quint16 num = StunUtil::read16((const quint8 *)encoded.data());
	quint16 len = StunUtil::read16((const quint8 *)encoded.data() + 2);
	if(encoded.size() - 4 < (int)len)
		return QByteArray();

	if(!d->getAddressPort(num, addr, port))
		return QByteArray();

	return encoded.mid(4, len);
}

QByteArray StunAllocate::decode(const StunMessage &encoded, QHostAddress *addr, int *port)
{
	QHostAddress paddr;
	quint16 pport;

	if(!StunTypes::parseXorPeerAddress(encoded.attribute(StunTypes::XOR_PEER_ADDRESS), encoded.magic(), encoded.id(), &paddr, &pport))
		return QByteArray();

	QByteArray data = encoded.attribute(StunTypes::DATA);
	if(data.isNull())
		return QByteArray();

	*addr = paddr;
	*port = pport;
	return data;
}

QString StunAllocate::errorString() const
{
	return d->errorString;
}

bool StunAllocate::containsChannelData(const quint8 *data, int size)
{
	return (check_channelData(data, size) != -1 ? true : false);
}

QByteArray StunAllocate::readChannelData(const quint8 *data, int size)
{
	int len = check_channelData(data, size);
	if(len != -1)
		return QByteArray((const char *)data, len);
	else
		return QByteArray();
}

}

#include "stunallocate.moc"
