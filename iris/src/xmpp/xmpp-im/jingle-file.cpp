/*
 * jignle-file.h - Jingle file usually used in file transfer
 * Copyright (C) 2019  Sergey Ilinykh
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

#include "jingle-file.h"

#include "xmpp_xmlcommon.h"

#include <QDomDocument>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

namespace XMPP::Jingle::FileTransfer {

const QString NS = QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5");

static const QString THUMBNAIL_TAG  = QStringLiteral("thumbnail");
static const QString RANGE_TAG      = QStringLiteral("range");
static const QString DATE_TAG       = QStringLiteral("date");
static const QString DESC_TAG       = QStringLiteral("desc");
static const QString MEDIA_TYPE_TAG = QStringLiteral("media-type");
static const QString NAME_TAG       = QStringLiteral("name");
static const QString SIZE_TAG       = QStringLiteral("size");
static const QString FILETAG        = QStringLiteral("file");
static const QString AMPLITUDES_TAG = QStringLiteral("amplitudes");

const QString AMPLITUDES_NS = QStringLiteral("urn:audio:amplitudes");

QDomElement Range::toXml(QDomDocument *doc) const
{
    auto r = doc->createElement(RANGE_TAG);
    if (length) {
        r.setAttribute(QStringLiteral("length"), QString::number(length));
    }
    if (offset) {
        r.setAttribute(QStringLiteral("offset"), QString::number(offset));
    }
    for (auto const &h : hashes) {
        auto hel = h.toXml(doc);
        if (!hel.isNull()) {
            r.appendChild(hel);
        }
    }
    return r;
}

//----------------------------------------------------------------------------
// File
//----------------------------------------------------------------------------
class File::Private : public QSharedData {
public:
    bool          rangeSupported = false;
    bool          hasSize        = false;
    QDateTime     date;
    QString       mediaType;
    QString       name;
    QString       desc;
    std::uint64_t size = 0;
    Range         range;
    QList<Hash>   hashes;
    Thumbnail     thumbnail;
    QByteArray    amplitudes;
};

File::File() { }

File::~File() { }

File &File::operator=(const File &other)
{
    d = other.d;
    return *this;
}

File::File(const File &other) : d(other.d) { }

File::File(const QDomElement &file)
{
    QDateTime     date;
    QString       mediaType;
    QString       name;
    QString       desc;
    std::uint64_t size           = 0;
    bool          rangeSupported = false;
    bool          hasSize        = false;
    Range         range;
    QList<Hash>   hashes;
    Thumbnail     thumbnail;
    QByteArray    amplitudes;

    bool ok;

    for (QDomElement ce = file.firstChildElement(); !ce.isNull(); ce = ce.nextSiblingElement()) {

        if (ce.tagName() == DATE_TAG) {
            date = QDateTime::fromString(ce.text().left(19), Qt::ISODate);
            if (!date.isValid()) {
                return;
            }

        } else if (ce.tagName() == MEDIA_TYPE_TAG) {
            mediaType = ce.text();

        } else if (ce.tagName() == NAME_TAG) {
            name = ce.text();

        } else if (ce.tagName() == SIZE_TAG) {
            size = ce.text().toULongLong(&ok);
            if (!ok) {
                return;
            }
            hasSize = true;

        } else if (ce.tagName() == RANGE_TAG) {
            if (ce.hasAttribute(QLatin1String("offset"))) {
                range.offset = ce.attribute(QLatin1String("offset")).toLongLong(&ok);
                if (!ok || range.offset < 0) {
                    return;
                }
            }
            if (ce.hasAttribute(QLatin1String("length"))) {
                range.length = ce.attribute(QLatin1String("length")).toLongLong(&ok);
                if (!ok || range.length <= 0) { // length should absent if we need to read till end of file.
                    // 0-length is nonsense
                    return;
                }
            }
            QDomElement hashEl = ce.firstChildElement(QLatin1String("hash"));
            for (; !hashEl.isNull(); hashEl = hashEl.nextSiblingElement(QLatin1String("hash"))) {
                if (hashEl.namespaceURI() == HASH_NS) {
                    auto hash = Hash(hashEl);
                    if (hash.type() == Hash::Type::Unknown) {
                        continue;
                    }
                    range.hashes.append(hash);
                }
            }
            rangeSupported = true;

        } else if (ce.tagName() == DESC_TAG) {
            desc = ce.text();

        } else if (ce.tagName() == QLatin1String("hash")) {
            if (ce.namespaceURI() == HASH_NS) {
                Hash h(ce);
                if (h.type() == Hash::Type::Unknown) {
                    return;
                }
                hashes.append(h);
            }

        } else if (ce.tagName() == QLatin1String("hash-used")) {
            if (ce.namespaceURI() == HASH_NS) {
                Hash h(ce);
                if (h.type() == Hash::Type::Unknown) {
                    return;
                }
                hashes.append(h);
            }

        } else if (ce.tagName() == THUMBNAIL_TAG) {
            thumbnail = Thumbnail(ce);
        } else if (ce.tagName() == AMPLITUDES_TAG && ce.namespaceURI() == AMPLITUDES_NS) {
            amplitudes = QByteArray::fromBase64(ce.text().toLatin1());
        }
    }

    auto p            = new Private;
    p->date           = date;
    p->mediaType      = mediaType;
    p->name           = name;
    p->desc           = desc;
    p->size           = size;
    p->rangeSupported = rangeSupported;
    p->hasSize        = hasSize;
    p->range          = range;
    p->hashes         = hashes;
    p->thumbnail      = thumbnail;
    p->amplitudes     = amplitudes;

    d = p;
}

QDomElement File::toXml(QDomDocument *doc) const
{
    if (!isValid() || d->hashes.isEmpty()) {
        return QDomElement();
    }
    QDomElement el = doc->createElementNS(NS, QStringLiteral("file"));
    if (d->date.isValid()) {
        el.appendChild(XMLHelper::textTag(*doc, DATE_TAG, d->date.toString(Qt::ISODate)));
    }
    if (d->desc.size()) {
        el.appendChild(XMLHelper::textTag(*doc, DESC_TAG, d->desc));
    }
    for (const auto &h : d->hashes) {
        el.appendChild(h.toXml(doc));
    }
    if (d->mediaType.size()) {
        el.appendChild(XMLHelper::textTag(*doc, MEDIA_TYPE_TAG, d->mediaType));
    }
    if (d->name.size()) {
        el.appendChild(XMLHelper::textTag(*doc, NAME_TAG, d->name));
    }
    if (d->hasSize) {
        el.appendChild(XMLHelper::textTag(*doc, SIZE_TAG, qint64(d->size)));
    }
    if (d->rangeSupported || d->range.isValid()) {
        el.appendChild(d->range.toXml(doc));
    }
    if (d->thumbnail.isValid()) {
        el.appendChild(d->thumbnail.toXml(doc));
    }
    if (d->amplitudes.size()) {
        el.appendChild(XMLHelper::textTagNS(doc, AMPLITUDES_NS, AMPLITUDES_TAG, d->amplitudes));
    }
    return el;
}

bool File::merge(const File &other)
{
    if (!d->thumbnail.isValid()) {
        d->thumbnail = other.thumbnail();
    }
    for (auto const &h : other.d->hashes) {
        auto it = std::find_if(d->hashes.constBegin(), d->hashes.constEnd(),
                               [&h](auto const &v) { return h.type() == v.type(); });
        if (it == d->hashes.constEnd()) {
            d->hashes.append(h);
        } else if (h.data() != it->data()) {
            return false; // hashes are different
        }
    }
    return true;
}

bool File::hasComputedHashes() const
{
    if (!d)
        return false;
    for (auto const &h : d->hashes) {
        if (h.data().size())
            return true;
    }
    return false;
}

bool File::hasSize() const { return d->hasSize; }

QDateTime File::date() const { return d ? d->date : QDateTime(); }

QString File::description() const { return d ? d->desc : QString(); }

QList<Hash> File::hashes() const { return d ? d->hashes : QList<Hash>(); }
QList<Hash> File::computedHashes() const
{
    QList<Hash> ret;
    if (!d)
        return ret;
    for (auto const &h : d->hashes) {
        if (h.data().size())
            ret.append(h);
    }
    return ret;
}

Hash File::hash(Hash::Type t) const
{
    if (d && d->hashes.count()) {
        if (t == Hash::Unknown)
            return d->hashes.at(0);
        for (auto const &h : d->hashes) {
            if (h.type() == t) {
                return h;
            }
        }
    }
    return Hash();
}

QString File::mediaType() const { return d ? d->mediaType : QString(); }

QString File::name() const { return d ? d->name : QString(); }

std::uint64_t File::size() const { return d ? d->size : 0; }

Range File::range() const { return d ? d->range : Range(); }

Thumbnail File::thumbnail() const { return d ? d->thumbnail : Thumbnail(); }

QByteArray File::amplitudes() const { return d ? d->amplitudes : QByteArray(); }

void File::setDate(const QDateTime &date) { ensureD()->date = date; }

void File::setDescription(const QString &desc) { ensureD()->desc = desc; }

void File::addHash(const Hash &hash) { ensureD()->hashes.append(hash); }

void File::setHashes(const QList<Hash> &hashes) { ensureD()->hashes = hashes; }

void File::setMediaType(const QString &mediaType) { ensureD()->mediaType = mediaType; }

void File::setName(const QString &name) { ensureD()->name = name; }

void File::setSize(std::uint64_t size)
{
    ensureD()->size = size;
    d->hasSize      = true;
}

void File::setRange(const Range &range)
{
    ensureD()->range  = range;
    d->rangeSupported = true;
}

void File::setThumbnail(const Thumbnail &thumb) { ensureD()->thumbnail = thumb; }

void File::setAmplitudes(const QByteArray &amplitudes) { d->amplitudes = amplitudes; }

File::Private *File::ensureD()
{
    if (!d) {
        d = new Private;
    }
    return d.data();
}

//----------------------------------------------------------------------------
// FileHasher
//----------------------------------------------------------------------------
class FileHasher::Private {
public:
    QThread    thread;
    StreamHash streamHash;
    Hash       result;

    Private(Hash::Type hashType) : streamHash(hashType) { }
};

FileHasher::FileHasher(Hash::Type type) : d(new Private(type))
{
    QSemaphore sem;
    moveToThread(&d->thread);
    QObject::connect(&d->thread, &QThread::started, this, [&sem]() { sem.release(); });
    d->thread.start();
    sem.acquire();
}

FileHasher::~FileHasher()
{
    if (d->thread.isRunning()) {
        addData(); // ensure exit called
        d->thread.wait();
    }
}

void FileHasher::addData(const QByteArray &data)
{
    QTimer::singleShot(0, this, [data, this]() {
        // executed in a thread
        d->streamHash.addData(data);
        if (data.isEmpty()) {
            d->result = d->streamHash.final();
            thread()->exit();
        }
    });
    if (data.isEmpty())
        d->thread.wait();
}

Hash FileHasher::result()
{
    if (d->thread.isRunning()) {
        addData(); // ensure exit called
        d->thread.wait();
    }
    return d->result;
}

}
