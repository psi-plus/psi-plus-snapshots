/*
 * filetransfer.h - File Transfer
 * Copyright (C) 2004  Justin Karneges
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

#ifndef XMPP_FILETRANSFER_H
#define XMPP_FILETRANSFER_H

#include "im.h"

namespace XMPP
{
	//class BSConnection;
	class BSConnection;
	class BytestreamManager;
	struct FTRequest;

	/*class AbstractFileTransfer
	{
		public:
			// Receive
			virtual Jid peer() const = 0;
			virtual QString fileName() const = 0;
			virtual qlonglong fileSize() const = 0;
			virtual QString description() const { return ""; }
			virtual bool rangeSupported() const { return false; }
			virtual void accept(qlonglong offset=0, qlonglong length=0) = 0;
	};*/

	class FTThumbnail
	{
	public:
		inline FTThumbnail() : width(0), height(0) {}
		// data - for outgoing it's actual image data. for incoming - cid
		inline FTThumbnail(const QByteArray &data,
						   const QString &mimeType = QString::null,
						   quint32 width = 0, quint32 height = 0) :
			data(data), mimeType(mimeType),
			width(width), height(height) { }
		inline bool isNull() const { return data.isNull(); }

		QByteArray data;
		QString mimeType;
		quint32 width;
		quint32 height;
	};

	class FileTransfer : public QObject /*, public AbstractFileTransfer */
	{
		Q_OBJECT
	public:
		enum { ErrReject, ErrNeg, ErrConnect, ErrProxy, ErrStream, Err400 };
		enum { Idle, Requesting, Connecting, WaitingForAccept, Active };
		~FileTransfer();

		FileTransfer *copy() const;

		void setProxy(const Jid &proxy);

		// send
		void sendFile(const Jid &to, const QString &fname, qlonglong size, const QString &desc, const FTThumbnail &thumb);
		qlonglong offset() const;
		qlonglong length() const;
		int dataSizeNeeded() const;
		void writeFileData(const QByteArray &a);
		const FTThumbnail &thumbnail() const;

		// receive
		Jid peer() const;
		QString fileName() const;
		qlonglong fileSize() const;
		QString description() const;
		bool rangeSupported() const;
		void accept(qlonglong offset=0, qlonglong length=0);

		// both
		void close(); // reject, or stop sending/receiving
		BSConnection *bsConnection() const; // active link

	signals:
		void accepted(); // indicates BSConnection has started
		void connected();
		void readyRead(const QByteArray &a);
		void bytesWritten(qint64);
		void error(int);

	private slots:
		void ft_finished();
		void stream_connected();
		void stream_connectionClosed();
		void stream_readyRead();
		void stream_bytesWritten(qint64);
		void stream_error(int);
		void doAccept();
		void reset();

	private:
		class Private;
		Private *d;

		friend class FileTransferManager;
		FileTransfer(FileTransferManager *, QObject *parent=0);
		FileTransfer(const FileTransfer& other);
		void man_waitForAccept(const FTRequest &req, const QString &streamType);
		void takeConnection(BSConnection *c);
	};

	class FileTransferManager : public QObject
	{
		Q_OBJECT
	public:
		FileTransferManager(Client *);
		~FileTransferManager();

		bool isActive(const FileTransfer *ft) const;
		void setDisabled(const QString &ns, bool state = true);

		Client *client() const;
		FileTransfer *createTransfer();
		FileTransfer *takeIncoming();

	signals:
		void incomingReady();

	private slots:
		void pft_incoming(const FTRequest &req);

	private:
		class Private;
		Private *d;

		friend class Client;
		void stream_incomingReady(BSConnection *);

		friend class FileTransfer;
		BytestreamManager* streamManager(const QString &ns) const;
		QStringList streamPriority() const;
		QString link(FileTransfer *);
		void con_accept(FileTransfer *);
		void con_reject(FileTransfer *);
		void unlink(FileTransfer *);
	};

	class JT_FT : public Task
	{
		Q_OBJECT
	public:
		JT_FT(Task *parent);
		~JT_FT();

		void request(const Jid &to, const QString &id, const QString &fname,
					 qlonglong size, const QString &desc,
					 const QStringList &streamTypes, const FTThumbnail &thumb);
		qlonglong rangeOffset() const;
		qlonglong rangeLength() const;
		QString streamType() const;

		void onGo();
		bool take(const QDomElement &);

	private:
		class Private;
		Private *d;
	};

	struct FTRequest
	{
		Jid from;
		QString iq_id, id;
		QString fname;
		qlonglong size;
		QString desc;
		bool rangeSupported;
		QStringList streamTypes;
		FTThumbnail thumbnail;
	};
	class JT_PushFT : public Task
	{
		Q_OBJECT
	public:
		JT_PushFT(Task *parent);
		~JT_PushFT();

		void respondSuccess(const Jid &to, const QString &id, qlonglong rangeOffset, qlonglong rangeLength, const QString &streamType);
		void respondError(const Jid &to, const QString &id,
						  Stanza::Error::ErrorCond cond, const QString &str);

		bool take(const QDomElement &);

	signals:
		void incoming(const FTRequest &req);
	};
}

#endif
