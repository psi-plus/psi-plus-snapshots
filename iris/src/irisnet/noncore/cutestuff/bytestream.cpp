/*
 * bytestream.cpp - base class for bytestreams
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

#include "bytestream.h"
#include <QByteArray>

// CS_NAMESPACE_BEGIN

//! \class ByteStream bytestream.h
//! \brief Base class for "bytestreams"
//!
//! This class provides a basic framework for a "bytestream", here defined
//! as a bi-directional, asynchronous pipe of data.  It can be used to create
//! several different kinds of bytestream-applications, such as a console or
//! TCP connection, or something more abstract like a security layer or tunnel,
//! all with the same interface.  The provided functions make creating such
//! classes simpler.  ByteStream is a pure-virtual class, so you do not use it
//! on its own, but instead through a subclass such as \a BSocket.
//!
//! The signals connectionClosed(), delayedCloseFinished(), readyRead(),
//! bytesWritten(), and error() serve the exact same function as those from
//! <A HREF="http://doc.trolltech.com/3.1/qsocket.html">QSocket</A>.
//!
//! The simplest way to create a ByteStream is to reimplement isOpen(), close(),
//! and tryWrite().  Call appendRead() whenever you want to make data available for
//! reading.  ByteStream will take care of the buffers with regards to the caller,
//! and will call tryWrite() when the write buffer gains data.  It will be your
//! job to call tryWrite() whenever it is acceptable to write more data to
//! the underlying system.
//!
//! If you need more advanced control, reimplement read(), write(), bytesAvailable(),
//! and/or bytesToWrite() as necessary.
//!
//! Use appendRead(), appendWrite(), takeRead(), and takeWrite() to modify the
//! buffers.  If you have more advanced requirements, the buffers can be accessed
//! directly with readBuf() and writeBuf().
//!
//! Also available are the static convenience functions ByteStream::appendArray()
//! and ByteStream::takeArray(), which make dealing with byte queues very easy.

class ByteStream::Private
{
public:
	Private() {}

	QByteArray readBuf, writeBuf;
	int errorCode;
	QString errorText;
};

//!
//! Constructs a ByteStream object with parent \a parent.
ByteStream::ByteStream(QObject *parent)
:QIODevice(parent)
{
	d = new Private;
}

//!
//! Destroys the object and frees allocated resources.
ByteStream::~ByteStream()
{
	delete d;
}

//!
//! Writes array \a a to the stream.
qint64 ByteStream::writeData(const char *data, qint64 maxSize)
{
	if(!isOpen())
		return - 1;

	bool doWrite = bytesToWrite() == 0 ? true: false;
	d->writeBuf.append(data, maxSize);
	if(doWrite)
		tryWrite();
	return maxSize;
}

//!
//! Reads bytes \a bytes of data from the stream and returns them as an array.  If \a bytes is 0, then
//! \a read will return all available data.
qint64 ByteStream::readData(char *data, qint64 maxSize)
{
	maxSize = maxSize > d->readBuf.size()? d->readBuf.size() : maxSize;
	memcpy(data, d->readBuf.constData(), maxSize);
	d->readBuf.remove(0, maxSize);
	return maxSize;
}

//!
//! Returns the number of bytes available for reading.
qint64 ByteStream::bytesAvailable() const
{
	return QIODevice::bytesAvailable() + d->readBuf.size();
}

//!
//! Returns the number of bytes that are waiting to be written.
qint64 ByteStream::bytesToWrite() const
{
	return d->writeBuf.size();
}

//!
//! Clears the read buffer.
void ByteStream::clearReadBuffer()
{
	d->readBuf.resize(0);
}

//!
//! Clears the write buffer.
void ByteStream::clearWriteBuffer()
{
	d->writeBuf.resize(0);
}

//!
//! Appends \a block to the end of the read buffer.
void ByteStream::appendRead(const QByteArray &block)
{
	d->readBuf += block;
}

//!
//! Appends \a block to the end of the write buffer.
void ByteStream::appendWrite(const QByteArray &block)
{
	d->writeBuf += block;
}

//!
//! Returns \a size bytes from the start of the read buffer.
//! If \a size is 0, then all available data will be returned.
//! If \a del is TRUE, then the bytes are also removed.
QByteArray ByteStream::takeRead(int size, bool del)
{
	return takeArray(d->readBuf, size, del);
}

//!
//! Returns \a size bytes from the start of the write buffer.
//! If \a size is 0, then all available data will be returned.
//! If \a del is TRUE, then the bytes are also removed.
QByteArray ByteStream::takeWrite(int size, bool del)
{
	return takeArray(d->writeBuf, size, del);
}

//!
//! Returns a reference to the read buffer.
QByteArray & ByteStream::readBuf()
{
	return d->readBuf;
}

//!
//! Returns a reference to the write buffer.
QByteArray & ByteStream::writeBuf()
{
	return d->writeBuf;
}

//!
//! Attempts to try and write some bytes from the write buffer, and returns the number
//! successfully written or -1 on error.  The default implementation returns -1.
int ByteStream::tryWrite()
{
	return -1;
}

//!
//! Returns \a size bytes from the start of the array pointed to by \a from.
//! If \a size is 0, then all available data will be returned.
//! If \a del is TRUE, then the bytes are also removed.
QByteArray ByteStream::takeArray(QByteArray &from, int size, bool del)
{
	QByteArray result;
	if(size == 0) {
		result = from;
		if(del)
			from.resize(0);
	}
	else {
		result = from.left(size);
		if (del) {
			from.remove(0, size);
		}
	}
	return result;
}

//!
//! Returns last error code.
int ByteStream::errorCode() const
{
	return d->errorCode;
}

//!
//! Returns last error string corresponding to last error code.
QString &ByteStream::errorText() const
{
	return d->errorText;
}

//!
//! Sets last error with \a code and \a text and emit it
void ByteStream::setError(int code, const QString &text)
{
	d->errorCode = code;
	d->errorText = text;
	if (code != ErrOk) {
		emit error(code);
	}
}

	void connectionClosed();
	void delayedCloseFinished();
	void readyRead();
	void bytesWritten(qint64);
	void error(int);

//! \fn void ByteStream::connectionClosed()
//! This signal is emitted when the remote end of the stream closes.

//! \fn void ByteStream::delayedCloseFinished()
//! This signal is emitted when all pending data has been written to the stream
//! after an attempt to close.

//! \fn void ByteStream::readyRead()
//! This signal is emitted when data is available to be read.

//! \fn void ByteStream::error(int code)
//! This signal is emitted when an error occurs in the stream.  The reason for
//! error is indicated by \a code.

// CS_NAMESPACE_END
