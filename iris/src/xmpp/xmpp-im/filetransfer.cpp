/*
 * filetransfer.cpp - File Transfer
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

#include "filetransfer.h"

#include <QList>
#include <QTimer>
#include <QPointer>
#include <QFileInfo>
#include <QSet>
#include "xmpp_xmlcommon.h"
#include "s5b.h"
#include "xmpp_ibb.h"

#define SENDBUFSIZE 65536

using namespace XMPP;

// firstChildElement
//
// Get an element's first child element
static QDomElement firstChildElement(const QDomElement &e)
{
	for(QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
		if(n.isElement())
			return n.toElement();
	}
	return QDomElement();
}

//----------------------------------------------------------------------------
// FileTransfer
//----------------------------------------------------------------------------
class FileTransfer::Private
{
public:
	FileTransferManager *m;
	JT_FT *ft;
	Jid peer;
	QString fname;
	qlonglong size;
	qlonglong sent;
	QString desc;
	bool rangeSupported;
	qlonglong rangeOffset, rangeLength, length;
	QString streamType;
	FTThumbnail thumbnail;
	bool needStream;
	QString id, iq_id;
	BSConnection *c;
	Jid proxy;
	int state;
	bool sender;
};

FileTransfer::FileTransfer(FileTransferManager *m, QObject *parent)
:QObject(parent)
{
	d = new Private;
	d->m = m;
	d->ft = 0;
	d->c = 0;
	reset();
}

FileTransfer::FileTransfer(const FileTransfer& other)
	: QObject(other.parent())
{
	d = new Private;
	*d = *other.d;
	d->m = other.d->m;
	d->ft = 0;
	d->c = 0;
	reset();

	if (d->m->isActive(&other))
		d->m->link(this);
}

FileTransfer::~FileTransfer()
{
	reset();
	delete d;
}

FileTransfer *FileTransfer::copy() const
{
	return new FileTransfer(*this);
}

void FileTransfer::reset()
{
	d->m->unlink(this);

	delete d->ft;
	d->ft = 0;

	if (d->c) {
		d->c->disconnect(this);
		d->c->manager()->deleteConnection(d->c, d->state == Active && !d->sender ?
											  3000 : 0);
		d->c = 0;
	}

	d->state = Idle;
	d->needStream = false;
	d->sent = 0;
	d->sender = false;
}

void FileTransfer::setProxy(const Jid &proxy)
{
	d->proxy = proxy;
}

void FileTransfer::sendFile(const Jid &to, const QString &fname, qlonglong size,
							const QString &desc, const FTThumbnail &thumb)
{
	d->state = Requesting;
	d->peer = to;
	d->fname = fname;
	d->size = size;
	d->desc = desc;
	d->sender = true;
	d->id = d->m->link(this);

	d->ft = new JT_FT(d->m->client()->rootTask());
	connect(d->ft, SIGNAL(finished()), SLOT(ft_finished()));
	d->ft->request(to, d->id, fname, size, desc, d->m->streamPriority(), thumb);
	d->ft->go(true);
}

int FileTransfer::dataSizeNeeded() const
{
	int pending = d->c->bytesToWrite();
	if(pending >= SENDBUFSIZE)
		return 0;
	qlonglong left = d->length - (d->sent + pending);
	int size = SENDBUFSIZE - pending;
	if((qlonglong)size > left)
		size = (int)left;
	return size;
}

void FileTransfer::writeFileData(const QByteArray &a)
{
	int pending = d->c->bytesToWrite();
	qlonglong left = d->length - (d->sent + pending);
	if(left == 0)
		return;

	QByteArray block;
	if((qlonglong)a.size() > left) {
		block = a;
		block.resize((uint)left);
	}
	else
		block = a;
	d->c->write(block);
}

const FTThumbnail &FileTransfer::thumbnail() const
{
	return d->thumbnail;
}

Jid FileTransfer::peer() const
{
	return d->peer;
}

QString FileTransfer::fileName() const
{
	return d->fname;
}

qlonglong FileTransfer::fileSize() const
{
	return d->size;
}

QString FileTransfer::description() const
{
	return d->desc;
}

bool FileTransfer::rangeSupported() const
{
	return d->rangeSupported;
}

qlonglong FileTransfer::offset() const
{
	return d->rangeOffset;
}

qlonglong FileTransfer::length() const
{
	return d->length;
}

void FileTransfer::accept(qlonglong offset, qlonglong length)
{
	d->state = Connecting;
	d->rangeOffset = offset;
	d->rangeLength = length;
	if(length > 0)
		d->length = length;
	else
		d->length = d->size;
	d->m->con_accept(this);
}

void FileTransfer::close()
{
	if(d->state == Idle)
		return;
	if(d->state == WaitingForAccept)
		d->m->con_reject(this);
	else if(d->state == Active)
		d->c->close();
	reset();
}

BSConnection *FileTransfer::bsConnection() const
{
	return d->c;
}

// file transfer request accepted or error happened
void FileTransfer::ft_finished()
{
	JT_FT *ft = d->ft;
	d->ft = 0;

	if(ft->success()) {
		d->state = Connecting;
		d->rangeOffset = ft->rangeOffset();
		d->length = ft->rangeLength();
		if(d->length == 0)
			d->length = d->size - d->rangeOffset;
		d->streamType = ft->streamType();
		BytestreamManager *streamManager = d->m->streamManager(d->streamType);
		if (streamManager) {
			d->c = streamManager->createConnection();
			if (dynamic_cast<S5BManager*>(streamManager) && d->proxy.isValid()) {
				((S5BConnection*)(d->c))->setProxy(d->proxy);
			}
			connect(d->c, SIGNAL(connected()), SLOT(stream_connected()));
			connect(d->c, SIGNAL(connectionClosed()), SLOT(stream_connectionClosed()));
			connect(d->c, SIGNAL(bytesWritten(qint64)), SLOT(stream_bytesWritten(qint64)));
			connect(d->c, SIGNAL(error(int)), SLOT(stream_error(int)));

			d->c->connectToJid(d->peer, d->id);
			accepted();
		}
		else {
			emit error(Err400);
			reset();
		}
	}
	else {
		if(ft->statusCode() == 403)
			emit error(ErrReject);
		else if(ft->statusCode() == 400)
			emit error(Err400);
		else
			emit error(ErrNeg);
		reset();
	}
}

void FileTransfer::takeConnection(BSConnection *c)
{
	d->c = c;
	connect(d->c, SIGNAL(connected()), SLOT(stream_connected()));
	connect(d->c, SIGNAL(connectionClosed()), SLOT(stream_connectionClosed()));
	connect(d->c, SIGNAL(readyRead()), SLOT(stream_readyRead()));
	connect(d->c, SIGNAL(error(int)), SLOT(stream_error(int)));

	S5BConnection *s5b = dynamic_cast<S5BConnection*>(c);
	if(s5b && d->proxy.isValid())
		s5b->setProxy(d->proxy);
	accepted();
	QTimer::singleShot(0, this, SLOT(doAccept()));
}

void FileTransfer::stream_connected()
{
	d->state = Active;
	emit connected();
}

void FileTransfer::stream_connectionClosed()
{
	reset();
	emit error(ErrStream);
}

void FileTransfer::stream_readyRead()
{
	QByteArray a = d->c->readAll();
	qlonglong need = d->length - d->sent;
	if((qlonglong)a.size() > need)
		a.resize((uint)need);
	d->sent += a.size();
	if(d->sent == d->length)
		reset();
	readyRead(a);
}

void FileTransfer::stream_bytesWritten(qint64 x)
{
	d->sent += x;
	if(d->sent == d->length)
		reset();
	emit bytesWritten(x);
}

void FileTransfer::stream_error(int x)
{
	reset();
	if(x == BSConnection::ErrRefused || x == BSConnection::ErrConnect)
		error(ErrConnect);
	else if(x == BSConnection::ErrProxy)
		error(ErrProxy);
	else
		error(ErrStream);
}

void FileTransfer::man_waitForAccept(const FTRequest &req, const QString &streamType)
{
	d->state = WaitingForAccept;
	d->peer = req.from;
	d->id = req.id;
	d->iq_id = req.iq_id;
	d->fname = req.fname;
	d->size = req.size;
	d->desc = req.desc;
	d->rangeSupported = req.rangeSupported;
	d->streamType = streamType;
	d->thumbnail = req.thumbnail;
}

void FileTransfer::doAccept()
{
	d->c->accept();
}

//----------------------------------------------------------------------------
// FileTransferManager
//----------------------------------------------------------------------------
class FileTransferManager::Private
{
public:
	Client *client;
	QList<FileTransfer*> list, incoming;
	QStringList streamPriority;
	QHash<QString, BytestreamManager*> streamMap;
	QSet<QString> disabledStreamTypes;
	JT_PushFT *pft;
};

FileTransferManager::FileTransferManager(Client *client)
:QObject(client)
{
	d = new Private;
	d->client = client;
	if (client->s5bManager()) {
		d->streamPriority.append(S5BManager::ns());
		d->streamMap[S5BManager::ns()] = client->s5bManager();
	}
	if (client->ibbManager()) {
		d->streamPriority.append(IBBManager::ns());
		d->streamMap[IBBManager::ns()] = client->ibbManager();
	}

	d->pft = new JT_PushFT(d->client->rootTask());
	connect(d->pft, SIGNAL(incoming(FTRequest)), SLOT(pft_incoming(FTRequest)));
}

FileTransferManager::~FileTransferManager()
{
	while (!d->incoming.isEmpty()) {
		delete d->incoming.takeFirst();
	}
	delete d->pft;
	delete d;
}

Client *FileTransferManager::client() const
{
	return d->client;
}

FileTransfer *FileTransferManager::createTransfer()
{
	FileTransfer *ft = new FileTransfer(this);
	return ft;
}

FileTransfer *FileTransferManager::takeIncoming()
{
	if(d->incoming.isEmpty())
		return 0;

	FileTransfer *ft = d->incoming.takeFirst();

	// move to active list
	d->list.append(ft);
	return ft;
}

bool FileTransferManager::isActive(const FileTransfer *ft) const
{
	return d->list.contains(const_cast<FileTransfer*>(ft)) > 0;
}

void FileTransferManager::setDisabled(const QString &ns, bool state)
{
	if (state) {
		d->disabledStreamTypes.insert(ns);
	}
	else {
		d->disabledStreamTypes.remove(ns);
	}
}

void FileTransferManager::pft_incoming(const FTRequest &req)
{
	QString streamType;
	foreach(const QString& ns, d->streamPriority) {
		if(req.streamTypes.contains(ns)) {
			BytestreamManager *manager = streamManager(ns);
			if (manager && manager->isAcceptableSID(req.from, req.id)) {
				streamType = ns;
				break;
			}
		}
	}

	if(streamType.isEmpty()) {
		d->pft->respondError(req.from, req.iq_id, Stanza::Error::NotAcceptable,
							 "No valid stream types");
		return;
	}

	FileTransfer *ft = new FileTransfer(this);
	ft->man_waitForAccept(req, streamType);
	d->incoming.append(ft);
	incomingReady();
}

BytestreamManager* FileTransferManager::streamManager(const QString &ns) const
{
	if (d->disabledStreamTypes.contains(ns)) {
		return 0;
	}
	return d->streamMap.value(ns);
}

QStringList FileTransferManager::streamPriority() const
{
	QStringList ret;
	foreach (const QString &ns, d->streamPriority) {
		if (!d->disabledStreamTypes.contains(ns)) {
			ret.append(ns);
		}
	}
	return ret;
}

void FileTransferManager::stream_incomingReady(BSConnection *c)
{
	foreach(FileTransfer* ft, d->list) {
		if(ft->d->needStream && ft->d->peer.compare(c->peer()) && ft->d->id == c->sid()) {
			ft->takeConnection(c);
			return;
		}
	}
	c->close();
	delete c;
}

QString FileTransferManager::link(FileTransfer *ft)
{
	QString id;
	bool found;
	do {
		found = false;
		id = QString("ft_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
		foreach (FileTransfer* ft, d->list) {
			if (ft->d->peer.compare(ft->d->peer) && ft->d->id == id) {
				found = true;
				break;
			}
		}
	} while(found);
	d->list.append(ft);
	return id;
}

void FileTransferManager::con_accept(FileTransfer *ft)
{
	ft->d->needStream = true;
	d->pft->respondSuccess(ft->d->peer, ft->d->iq_id, ft->d->rangeOffset, ft->d->rangeLength, ft->d->streamType);
}

void FileTransferManager::con_reject(FileTransfer *ft)
{
	d->pft->respondError(ft->d->peer, ft->d->iq_id, Stanza::Error::Forbidden, "Declined");
}

void FileTransferManager::unlink(FileTransfer *ft)
{
	d->list.removeAll(ft);
}

//----------------------------------------------------------------------------
// JT_FT
//----------------------------------------------------------------------------
class JT_FT::Private
{
public:
	QDomElement iq;
	Jid to;
	qlonglong size, rangeOffset, rangeLength;
	QString streamType;
	QStringList streamTypes;
};

JT_FT::JT_FT(Task *parent)
:Task(parent)
{
	d = new Private;
}

JT_FT::~JT_FT()
{
	delete d;
}

void JT_FT::request(const Jid &to, const QString &_id, const QString &fname,
					qlonglong size, const QString &desc,
					const QStringList &streamTypes, const FTThumbnail &thumb)
{
	QDomElement iq;
	d->to = to;
	iq = createIQ(doc(), "set", to.full(), id());
	QDomElement si = doc()->createElement("si");
	si.setAttribute("xmlns", "http://jabber.org/protocol/si");
	si.setAttribute("id", _id);
	si.setAttribute("profile", "http://jabber.org/protocol/si/profile/file-transfer");

	QDomElement file = doc()->createElement("file");
	file.setAttribute("xmlns", "http://jabber.org/protocol/si/profile/file-transfer");
	file.setAttribute("name", fname);
	file.setAttribute("size", QString::number(size));
	if(!desc.isEmpty()) {
		QDomElement de = doc()->createElement("desc");
		de.appendChild(doc()->createTextNode(desc));
		file.appendChild(de);
	}
	QDomElement range = doc()->createElement("range");
	file.appendChild(range);

	if (!thumb.data.isEmpty()) {
		BoBData data = client()->bobManager()->append(thumb.data, thumb.mimeType);
		QDomElement thel = doc()->createElement("thumbnail");
		thel.setAttribute("xmlns", "urn:xmpp:thumbs:0");
		thel.setAttribute("cid", data.cid());
		thel.setAttribute("mime-type", thumb.mimeType);
		if (thumb.width && thumb.height) {
			thel.setAttribute("width", thumb.width);
			thel.setAttribute("height", thumb.height);
		}
		file.appendChild(thel);
	}

	si.appendChild(file);

	QDomElement feature = doc()->createElement("feature");
	feature.setAttribute("xmlns", "http://jabber.org/protocol/feature-neg");
	QDomElement x = doc()->createElement("x");
	x.setAttribute("xmlns", "jabber:x:data");
	x.setAttribute("type", "form");

	QDomElement field = doc()->createElement("field");
	field.setAttribute("var", "stream-method");
	field.setAttribute("type", "list-single");
	for(QStringList::ConstIterator it = streamTypes.begin(); it != streamTypes.end(); ++it) {
		QDomElement option = doc()->createElement("option");
		QDomElement value = doc()->createElement("value");
		value.appendChild(doc()->createTextNode(*it));
		option.appendChild(value);
		field.appendChild(option);
	}

	x.appendChild(field);
	feature.appendChild(x);

	si.appendChild(feature);
	iq.appendChild(si);

	d->streamTypes = streamTypes;
	d->size = size;
	d->iq = iq;
}

qlonglong JT_FT::rangeOffset() const
{
	return d->rangeOffset;
}

qlonglong JT_FT::rangeLength() const
{
	return d->rangeLength;
}

QString JT_FT::streamType() const
{
	return d->streamType;
}

void JT_FT::onGo()
{
	send(d->iq);
}

bool JT_FT::take(const QDomElement &x)
{
	if(!iqVerify(x, d->to, id()))
		return false;

	if(x.attribute("type") == "result") {
		QDomElement si = firstChildElement(x);
		if(si.attribute("xmlns") != "http://jabber.org/protocol/si" || si.tagName() != "si") {
			setError(900, "");
			return true;
		}

		QString id = si.attribute("id");

		qlonglong range_offset = 0;
		qlonglong range_length = 0;

		QDomElement file = si.elementsByTagName("file").item(0).toElement();
		if(!file.isNull()) {
			QDomElement range = file.elementsByTagName("range").item(0).toElement();
			if(!range.isNull()) {
				qlonglong x;
				bool ok;
				if(range.hasAttribute("offset")) {
					x = range.attribute("offset").toLongLong(&ok);
					if(!ok || x < 0) {
						setError(900, "");
						return true;
					}
					range_offset = x;
				}
				if(range.hasAttribute("length")) {
					x = range.attribute("length").toLongLong(&ok);
					if(!ok || x < 0) {
						setError(900, "");
						return true;
					}
					range_length = x;
				}
			}
		}

		if(range_offset > d->size || (range_length > (d->size - range_offset))) {
			setError(900, "");
			return true;
		}

		QString streamtype;
		QDomElement feature = si.elementsByTagName("feature").item(0).toElement();
		if(!feature.isNull() && feature.attribute("xmlns") == "http://jabber.org/protocol/feature-neg") {
			QDomElement x = feature.elementsByTagName("x").item(0).toElement();
			if(!x.isNull() && x.attribute("type") == "submit") {
				QDomElement field = x.elementsByTagName("field").item(0).toElement();
				if(!field.isNull() && field.attribute("var") == "stream-method") {
					QDomElement value = field.elementsByTagName("value").item(0).toElement();
					if(!value.isNull())
						streamtype = value.text();
				}
			}
		}

		// must be one of the offered streamtypes
		if (!d->streamTypes.contains(streamtype)) {
			return true;
		}

		d->rangeOffset = range_offset;
		d->rangeLength = range_length;
		d->streamType = streamtype;
		setSuccess();
	}
	else {
		setError(x);
	}

	return true;
}

//----------------------------------------------------------------------------
// JT_PushFT
//----------------------------------------------------------------------------
JT_PushFT::JT_PushFT(Task *parent)
:Task(parent)
{
}

JT_PushFT::~JT_PushFT()
{
}

void JT_PushFT::respondSuccess(const Jid &to, const QString &id, qlonglong rangeOffset, qlonglong rangeLength, const QString &streamType)
{
	QDomElement iq = createIQ(doc(), "result", to.full(), id);
	QDomElement si = doc()->createElement("si");
	si.setAttribute("xmlns", "http://jabber.org/protocol/si");

	if(rangeOffset != 0 || rangeLength != 0) {
		QDomElement file = doc()->createElement("file");
		file.setAttribute("xmlns", "http://jabber.org/protocol/si/profile/file-transfer");
		QDomElement range = doc()->createElement("range");
		if(rangeOffset > 0)
			range.setAttribute("offset", QString::number(rangeOffset));
		if(rangeLength > 0)
			range.setAttribute("length", QString::number(rangeLength));
		file.appendChild(range);
		si.appendChild(file);
	}

	QDomElement feature = doc()->createElement("feature");
	feature.setAttribute("xmlns", "http://jabber.org/protocol/feature-neg");
	QDomElement x = doc()->createElement("x");
	x.setAttribute("xmlns", "jabber:x:data");
	x.setAttribute("type", "submit");

	QDomElement field = doc()->createElement("field");
	field.setAttribute("var", "stream-method");
	QDomElement value = doc()->createElement("value");
	value.appendChild(doc()->createTextNode(streamType));
	field.appendChild(value);

	x.appendChild(field);
	feature.appendChild(x);

	si.appendChild(feature);
	iq.appendChild(si);
	send(iq);
}

void JT_PushFT::respondError(const Jid &to, const QString &id,
							 Stanza::Error::ErrorCond cond, const QString &str)
{
	QDomElement iq = createIQ(doc(), "error", to.full(), id);
	Stanza::Error error(Stanza::Error::Cancel, cond, str);
	iq.appendChild(error.toXml(*client()->doc(), client()->stream().baseNS()));
	send(iq);
}

bool JT_PushFT::take(const QDomElement &e)
{
	// must be an iq-set tag
	if(e.tagName() != "iq")
		return false;
	if(e.attribute("type") != "set")
		return false;

	QDomElement si = firstChildElement(e);
	if(si.attribute("xmlns") != "http://jabber.org/protocol/si" || si.tagName() != "si")
		return false;
	if(si.attribute("profile") != "http://jabber.org/protocol/si/profile/file-transfer")
		return false;

	Jid from(e.attribute("from"));
	QString id = si.attribute("id");

	QDomElement file = si.elementsByTagName("file").item(0).toElement();
	if(file.isNull())
		return true;

	QString fname = file.attribute("name");
	if(fname.isEmpty()) {
		respondError(from, id, Stanza::Error::BadRequest, "Bad file name");
		return true;
	}

	// ensure kosher
	{
		QFileInfo fi(fname);
		fname = fi.fileName();
	}

	bool ok;
	qlonglong size = file.attribute("size").toLongLong(&ok);
	if(!ok || size < 0) {
		respondError(from, id, Stanza::Error::BadRequest, "Bad file size");
		return true;
	}

	QString desc;
	QDomElement de = file.elementsByTagName("desc").item(0).toElement();
	if(!de.isNull())
		desc = de.text();

	bool rangeSupported = false;
	QDomElement range = file.elementsByTagName("range").item(0).toElement();
	if(!range.isNull())
		rangeSupported = true;

	QStringList streamTypes;
	QDomElement feature = si.elementsByTagName("feature").item(0).toElement();
	if(!feature.isNull() && feature.attribute("xmlns") == "http://jabber.org/protocol/feature-neg") {
		QDomElement x = feature.elementsByTagName("x").item(0).toElement();
		if(!x.isNull() /*&& x.attribute("type") == "form"*/) {
			QDomElement field = x.elementsByTagName("field").item(0).toElement();
			if(!field.isNull() && field.attribute("var") == "stream-method" && field.attribute("type") == "list-single") {
				QDomNodeList nl = field.elementsByTagName("option");
				for(int n = 0; n < nl.count(); ++n) {
					QDomElement e = nl.item(n).toElement();
					QDomElement value = e.elementsByTagName("value").item(0).toElement();
					if(!value.isNull())
						streamTypes += value.text();
				}
			}
		}
	}

	FTThumbnail thumb;
	QDomElement thel = file.elementsByTagName("thumbnail").item(0).toElement();
	if(!thel.isNull() && thel.attribute("xmlns") == QLatin1String("urn:xmpp:thumbs:0")) {
		thumb.data = thel.attribute("cid").toUtf8();
		thumb.mimeType = thel.attribute("mime-type");
		thumb.width = thel.attribute("width").toUInt();
		thumb.height = thel.attribute("height").toUInt();
	}

	FTRequest r;
	r.from = from;
	r.iq_id = e.attribute("id");
	r.id = id;
	r.fname = fname;
	r.size = size;
	r.desc = desc;
	r.rangeSupported = rangeSupported;
	r.streamTypes = streamTypes;
	r.thumbnail = thumb;

	emit incoming(r);
	return true;
}
