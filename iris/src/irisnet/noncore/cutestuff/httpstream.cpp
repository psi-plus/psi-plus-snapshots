/*
 * httpstream.cpp - HTTP stream parser
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

#include "httpstream.h"
#include "bsocket.h"
#include "../../xmpp/zlib/zlibdecompressor.h"




//------------------------------------------------------------------------------
// LayerStream
//------------------------------------------------------------------------------
// implemented here just in case. usually should be reimplemented or not used
void LayerStream::writeIncoming(const QByteArray &data)
{
	handleOutData(data); // no processing in base class so move data out
}

void LayerStream::handleOutData(const QByteArray &data)
{
	if (_dataOutLayer) {
		_dataOutLayer->writeIncoming(data);
	} else {
		appendRead(data);
		emit readyRead();
	}
}



//--------------------------------------------
// GzipStream
//--------------------------------------------
class GzipStream : public LayerStream
{
	Q_OBJECT

	ZLibDecompressor *zDec;
	QBuffer uncompressed;

public:
	GzipStream(QObject *parent) :
		LayerStream(parent)
	{
		uncompressed.setBuffer(&readBuf());
		zDec = new ZLibDecompressor(&uncompressed);
		connect(&uncompressed, SIGNAL(bytesWritten(qint64)), SLOT(decompressedWritten(qint64)));
	}

	void writeIncoming(const QByteArray &data)
	{
		zDec->write(data);
	}

private slots:
	void decompressedWritten(qint64 size)
	{
		if (size) {
			if (_dataOutLayer) {
				_dataOutLayer->writeIncoming(uncompressed.buffer());
				uncompressed.buffer().clear();
				uncompressed.seek(0);
			} else {
				emit readyRead();
			}
		}
	}
};



//--------------------------------------------
// ChunkedStream
//--------------------------------------------
class ChunkedStream : public LayerStream
{
	Q_OBJECT

	enum State
	{
		Header,
		Body,
		BodyEnd,
		Trailer
	};

	bool sizeParsed;
	State _state;
	quint64 chunkSize;
	quint64 chunkBytesLeft; // bytes left to read for current chunk
	static const quint8 tmpBufSize = 12;
	QByteArray tmpBuffer;

public:
	ChunkedStream(QObject *parent) :
		LayerStream(parent),
		sizeParsed(false),
		_state(Header)
	{
		tmpBuffer.reserve(tmpBufSize);
	}

	void writeIncoming(const QByteArray &data)
	{
		int index;
		QByteArray tail = QByteArray::fromRawData(data.constData(), data.size());
		while (tail.size()) {
			switch (_state) {
			case Header:
			{
				quint8 lastHeaderSize = (quint8)tmpBuffer.size();
				quint8 bufFree = tmpBufSize - lastHeaderSize;
				tmpBuffer += ((int)bufFree > tail.size() ? tail : QByteArray::fromRawData(
														   tail.constData(), bufFree));
				if ((index = tmpBuffer.indexOf("\r\n")) == -1) {
					if (!bufFree) {
						setError(ErrRead, "String for chunk header is too long");
					}
					return;
				}
				tmpBuffer.resize(index);
				int unparsedOffset = tmpBuffer.size() + 2 - lastHeaderSize;
				tail = QByteArray::fromRawData(tail.constData() + unparsedOffset,
											   tail.size() - unparsedOffset);


				chunkSize = tmpBuffer.toInt(&sizeParsed, 16);
				if (!sizeParsed) {
					setError(ErrRead, "chunk size parse failed");
					return;
				}
				chunkBytesLeft = chunkSize;
				tmpBuffer.clear(); // should be clean to make BodyEnd working
				_state = chunkSize? Body : Trailer; // 0 means the end of response
				break;
			}
			case Body:
			{
				QByteArray r = readTail(tail, chunkBytesLeft);
				chunkBytesLeft -= r.size();
				handleOutData(r);
				if (chunkBytesLeft) {
					break; // no enough data to finish chunk read
				}
				_state = BodyEnd;
			}
			case BodyEnd:
				tmpBuffer.append(readTail(tail, 2 - tmpBuffer.size()));
				if (tmpBuffer.size() == 2) {
					if (tmpBuffer[0] != '\r' || tmpBuffer[1] != '\n') {
						setError(ErrRead, "no \r\n at chunk end");
						return;
					}
					_state = Header;
				}
				break;
			case Trailer:
				// TODO
				break;
			}
		}
	}

private:
	QByteArray readTail(QByteArray &tail, int bytes) const
	{
		int rb = qMin<int>(bytes, tail.size());
		QByteArray ret = QByteArray::fromRawData(tail.constData(), rb);
		tail = QByteArray::fromRawData(tail.constData() + rb, tail.size() - rb);
		return ret;
	}
};




//--------------------------------------------
// HttpStream
//
// This layer assumes it receives raw data right from tcp or decoded ssl
// and on out it has some http unrelated data (html for example or contents
// of some file.). Layer internally creates another layers pipeline to handle
// http compression and chunked data (maybe other encodings in the future)
//--------------------------------------------
void HttpStream::writeIncoming(const QByteArray &data)
{
	if (!data.size()) {
		return;
	}
	QByteArray realData;
	if (!headersReady) {
		int parsePos = qMax<int>(headersBuffer.size() - 3, 0);
		headersBuffer += data;
		if (headersBuffer.indexOf("\r\n\r\n", parsePos) == -1) {
			return;
		}
		if (parseHeaders(headersBuffer, parsePos)) {
			parsePos += 2;
			realData = QByteArray::fromRawData(headersBuffer.constData() + parsePos,
											   headersBuffer.size() - parsePos);
			headersReady = true;
			QByteArray header = headers.value("Content-Encoding").toLower();
			if (!header.isEmpty()) {
				QList<QByteArray> tes = header.split(',');
				while (tes.size()) {
					QByteArray lv = tes.takeLast().trimmed().toLower();
					if (lv == "gzip" || lv == "x-gzip" || lv == "deflate") {
						pipeLine.append(new GzipStream(this));
					}
				}
			}
			header = headers.value("Transfer-Encoding");
			if (!header.isEmpty()) {
				QList<QByteArray> tes = header.split(',');
				while (tes.size()) {
					QByteArray lv = tes.takeLast().trimmed().toLower();
					if (lv == "chunked") {
						pipeLine.append(new ChunkedStream(this));
					}
					if (lv == "gzip" || lv == "x-gzip" || lv == "deflate") {
						pipeLine.append(new GzipStream(this));
					}
				}
				headers.remove("Content-Length"); // by rfc2616 we have to ignore this header
			}
			if (pipeLine.count()) { // connect pipes
				for (int i = 0; i < pipeLine.count() - 1; i++) {
					pipeLine[i]->setDataOutLayer(pipeLine[i+1]);
				}
				connect(pipeLine.last(), SIGNAL(readyRead()), SLOT(pipeLine_readyReady()));
			}
			emit metaDataChanged();
		} else {
			qDebug("Invalid header: %s", headersBuffer.mid(
				parsePos, headersBuffer.indexOf("\r\n", parsePos)).data());
			setError(QNetworkReply::ProtocolFailure, "Invalid headers");
		}
	} else {
		realData = data;
	}
	if (realData.size()) {
		if (pipeLine.count()) {
			pipeLine[0]->writeIncoming(realData);
		} else {
			handleOutData(realData);
		}
	}
}

void HttpStream::pipeLine_readyReady()
{
	LayerStream *s = static_cast<LayerStream *>(sender());
	handleOutData(s->readAll());
}

bool HttpStream::parseHeaders(const QByteArray &buffer, int &pos)
{
	bool valid = true;
	bool statusRead = false;
	pos = 0;
	int endPos = 0;
	QByteArray lastKey;
	while ((endPos = buffer.indexOf("\r\n", pos)) != -1 && endPos != pos) {
		if (!statusRead) {
			QRegExp statusRE("^HTTP/(1.[01]) (\\d{3})( .*)?$");
			if (!statusRE.exactMatch(QString::fromLatin1(
								   buffer.constData() + pos, endPos - pos)))
			{
				valid = false;
				break;
			}
			httpVersion = statusRE.cap(1);
			statusCode = statusRE.cap(2).toInt();
			statusText = statusRE.cap(3).trimmed();
			statusRead = true;
		} else {
			QHash<QByteArray, QByteArray>::iterator it;
			if (buffer[pos] == ' ' || buffer[pos] == '\t') { // multiline value
				if (lastKey.isEmpty() || (it = headers.find(lastKey)) == headers.end()) {
					valid = false;
					break;
				}
				*it += ' ';
				*it += buffer.mid(pos, endPos - pos).trimmed();
			} else { // normal header line
				int sPos = buffer.indexOf(':', pos);
				if (sPos == -1 || sPos == pos || sPos > endPos) {
					valid = false;
					break;
				}
				QByteArray newKey = buffer.mid(pos, sPos - pos);
				QByteArray newValue = buffer.mid(sPos + 1, endPos - sPos - 1);
				if ((it = headers.find(newKey)) != headers.end()) { // by rfc we can combine so-named keys
					*it += ',';
					*it += newValue;
				} else {
					headers.insert(newKey, newValue);
					lastKey = newKey;
				}
			}
		}
		pos = endPos + 2;
	}
	return valid;
}

#include "httpstream.moc"
