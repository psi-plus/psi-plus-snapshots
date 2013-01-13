/*
 * httpstream.h - HTTP stream parser
 * Copyright (C) 2013 Il'inykh Sergey (rion)
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

#ifndef HTTPSTREAM_H
#define HTTPSTREAM_H

#include <QHash>

#include "bytestream.h"

/*

  Network layers:
  1) If layer has set out data layer than all processed data will be send to
  this out layer.
  for example. ssl layer has http as out layer. so ssl decoded data will be
  written to http layer (ssl layer calls writeIncoming of http layer).
  2) If out layer is not set then processed data written directly to read buffer
  and readyRead emitted.
  3) When out layer finish to process incoming data it will go to step 1

  Each layer connect readyRead signal of out layer to readyRead signal of itself
  in case it don't want to make some additional postprocessing. When some
  external entity starts reading by readyRead signal from top of layers stack,
  each layer checks if it has out layer and tries to read from this out layer.
  If there is no out layer (bottom of stack) it reads from own read buffer.
*/

class LayerStream : public ByteStream
{
	Q_OBJECT
public:
	inline LayerStream(QObject *parent) :
		ByteStream(parent),
		_dataOutLayer(NULL) { }

	/*
	  Sets source data layer for this layer.
	*/
	inline LayerStream* setDataOutLayer(LayerStream *dol)
	{
		return _dataOutLayer = dol;
	}

	// implemented here just in case. usually should be reimplemented or not used
	virtual void writeIncoming(const QByteArray &data);

protected:
	void handleOutData(const QByteArray &data);

protected:
	LayerStream *_dataOutLayer;
};


//--------------------------------------------
// HttpStream
//
// This layer assumes it receives raw data right from tcp or decoded ssl
// and on out it has some http unrelated data (html for example or contents
// of some file.). Layer internally creates another layers pipeline to handle
// http compression and chunked data (maybe other encodings in the future)
//--------------------------------------------
class HttpStream : public LayerStream
{
	Q_OBJECT

public:
	inline HttpStream(QObject *parent) :
		LayerStream(parent),
		headersReady(false) { }

	void writeIncoming(const QByteArray &data);

private slots:
	void pipeLine_readyReady();

private:
	bool parseHeaders(const QByteArray &buffer, int &pos);

signals:
	void metaDataChanged();

private:
	bool headersReady;
	quint16 statusCode;
	QString statusText;
	QString httpVersion;
	QByteArray headersBuffer;
	QList<LayerStream*> pipeLine;
	QHash<QByteArray, QByteArray> headers;

};


#endif // HTTPSTREAM_H
