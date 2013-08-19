/*
 * bytestream.h - base class for bytestreams
 * Copyright (C) 2003  Justin Karneges
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

#ifndef CS_BYTESTREAM_H
#define CS_BYTESTREAM_H

#include <QObject>
#include <QByteArray>
#include <QIODevice>

class QAbstractSocket;
// CS_NAMESPACE_BEGIN

// CS_EXPORT_BEGIN
class ByteStream : public QIODevice
{
	Q_OBJECT
public:
	enum Error { ErrOk, ErrRead, ErrWrite, ErrCustom = 10 };
	ByteStream(QObject *parent=0);
	~ByteStream()=0;

	bool isSequential() const { return true; }
	qint64 bytesAvailable() const;
	qint64 bytesToWrite() const;

	static QByteArray takeArray(QByteArray &from, int size=0, bool del=true);

	int errorCode() const;
	QString &errorText() const;

	virtual QAbstractSocket* abstractSocket() const { return 0; }

signals:
	void connectionClosed();
	void delayedCloseFinished();
	void error(int);

protected:
	qint64 writeData(const char *data, qint64 maxSize);
	qint64 readData(char *data, qint64 maxSize);

	void setError(int code = ErrOk, const QString &text = QString());
	void clearReadBuffer();
	void clearWriteBuffer();
	void appendRead(const QByteArray &);
	void appendWrite(const QByteArray &);
	QByteArray takeRead(int size=0, bool del=true);
	QByteArray takeWrite(int size=0, bool del=true);
	QByteArray & readBuf();
	QByteArray & writeBuf();
	virtual int tryWrite();

private:
//! \if _hide_doc_
	class Private;
	Private *d;
//! \endif
};
// CS_EXPORT_END

// CS_NAMESPACE_END

#endif
