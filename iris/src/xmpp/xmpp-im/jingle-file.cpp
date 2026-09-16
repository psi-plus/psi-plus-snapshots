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

const QString NS               = QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5");
const QString FILE_METADATA_NS = QStringLiteral("urn:xmpp:file:metadata:0");

static const QString THUMBNAIL_TAG  = QStringLiteral("thumbnail");
static const QString RANGE_TAG      = QStringLiteral("range");
static const QString DATE_TAG       = QStringLiteral("date");
static const QString DESC_TAG       = QStringLiteral("desc");
static const QString MEDIA_TYPE_TAG = QStringLiteral("media-type");
static const QString NAME_TAG       = QStringLiteral("name");
static const QString SIZE_TAG       = QStringLiteral("size");
static const QString FILETAG        = QStringLiteral("file");
static const QString AMPLITUDES_TAG = QStringLiteral("amplitudes");
static const QString WIDTH_TAG      = QStringLiteral("width");
static const QString HEIGHT_TAG     = QStringLiteral("height");
static const QString LENGTH_TAG     = QStringLiteral("length");

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
    bool                         rangeSupported = false;
    QDateTime                    date;
    QString                      mediaType;
    QString                      name;
    QString                      desc;
    QMap<QString, QString>       localizedDescriptions;
    std::optional<std::uint64_t> size;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::uint64_t> length;
    Range                        range;
    QList<Hash>                  hashes;
    Thumbnail                    thumbnail;
    QList<Thumbnail>             extraThumbnails;
    QByteArray                   amplitudes;
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
    QDateTime                    date;
    QString                      mediaType;
    QString                      name;
    QString                      desc;
    QMap<QString, QString>       localizedDescriptions;
    bool                         defaultDescriptionSeen = false;
    std::optional<std::uint64_t> size;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::uint64_t> length;
    bool                         rangeSupported = false;
    Range                        range;
    QList<Hash>                  hashes;
    Thumbnail                    thumbnail;
    QList<Thumbnail>             extraThumbnails;
    QByteArray                   amplitudes;

    bool ok;

    for (QDomElement ce = file.firstChildElement(); !ce.isNull(); ce = ce.nextSiblingElement()) {

        if (ce.localName() == DATE_TAG) {
            date = QDateTime::fromString(ce.text(), Qt::ISODate);
            if (!date.isValid()) {
                return;
            }

        } else if (ce.localName() == MEDIA_TYPE_TAG) {
            mediaType = ce.text();

        } else if (ce.localName() == NAME_TAG) {
            name = ce.text();

        } else if (ce.localName() == SIZE_TAG) {
            size = ce.text().toULongLong(&ok);
            if (!ok) {
                return;
            }

        } else if (ce.localName() == WIDTH_TAG) {
            width = ce.text().toUInt(&ok);
            if (!ok)
                return;

        } else if (ce.localName() == HEIGHT_TAG) {
            height = ce.text().toUInt(&ok);
            if (!ok)
                return;

        } else if (ce.localName() == LENGTH_TAG) {
            length = ce.text().toULongLong(&ok);
            if (!ok)
                return;

        } else if (ce.localName() == RANGE_TAG) {
            if (ce.hasAttribute(QLatin1String("offset"))) {
                const auto offset = ce.attribute(QLatin1String("offset")).toLongLong(&ok);
                if (!ok || offset < 0) {
                    return;
                }
                range.offset = static_cast<std::uint64_t>(offset);
            }
            if (ce.hasAttribute(QLatin1String("length"))) {
                const auto rangeLength = ce.attribute(QLatin1String("length")).toLongLong(&ok);
                if (!ok || rangeLength <= 0) { // length should absent if we need to read till end of file.
                    // 0-length is nonsense
                    return;
                }
                range.length = static_cast<std::uint64_t>(rangeLength);
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

        } else if (ce.localName() == DESC_TAG) {
            const auto language
                = ce.attributeNS(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("lang"));
            if (language.isEmpty()) {
                if (defaultDescriptionSeen)
                    return;
                defaultDescriptionSeen = true;
                desc                   = ce.text();
            } else {
                if (localizedDescriptions.contains(language))
                    return;
                localizedDescriptions.insert(language, ce.text());
            }

        } else if (ce.localName() == QLatin1String("hash")) {
            if (ce.namespaceURI() == HASH_NS) {
                Hash h(ce);
                if (h.type() == Hash::Type::Unknown) {
                    return;
                }
                hashes.append(h);
            }

        } else if (ce.localName() == QLatin1String("hash-used")) {
            if (ce.namespaceURI() == HASH_NS) {
                Hash h(ce);
                if (h.type() == Hash::Type::Unknown) {
                    return;
                }
                hashes.append(h);
            }

        } else if (ce.localName() == THUMBNAIL_TAG) {
            Thumbnail parsedThumbnail(ce);
            if (parsedThumbnail.isValid()) {
                if (!thumbnail.isValid())
                    thumbnail = parsedThumbnail;
                else
                    extraThumbnails.append(parsedThumbnail);
            }
        } else if (ce.localName() == AMPLITUDES_TAG && ce.namespaceURI() == AMPLITUDES_NS) {
            amplitudes = QByteArray::fromBase64(ce.text().toLatin1());
        }
    }

    auto p                   = new Private;
    p->date                  = date;
    p->mediaType             = mediaType;
    p->name                  = name;
    p->desc                  = desc;
    p->localizedDescriptions = localizedDescriptions;
    p->size                  = size;
    p->width                 = width;
    p->height                = height;
    p->length                = length;
    p->rangeSupported        = rangeSupported;
    p->range                 = range;
    p->hashes                = hashes;
    p->thumbnail             = thumbnail;
    p->extraThumbnails       = extraThumbnails;
    p->amplitudes            = amplitudes;

    d = p;
}

QDomElement File::toXml(QDomDocument *doc) const
{
    if (!isValid() || d->hashes.isEmpty())
        return {};
    return toXml(doc, NS, true);
}

QDomElement File::toMetadataXml(QDomDocument *doc) const { return toXml(doc, FILE_METADATA_NS, false); }

QDomElement File::toXml(QDomDocument *doc, const QString &ns, bool jingleExtensions) const
{
    if (!doc || !isValid())
        return {};

    QDomElement el = doc->createElementNS(ns, QStringLiteral("file"));
    if (d->date.isValid())
        el.appendChild(XMLHelper::textTag(*doc, DATE_TAG, d->date.toString(Qt::ISODate)));
    if (!d->desc.isEmpty())
        el.appendChild(XMLHelper::textTag(*doc, DESC_TAG, d->desc));
    if (!jingleExtensions) {
        for (auto it = d->localizedDescriptions.cbegin(); it != d->localizedDescriptions.cend(); ++it) {
            auto desc = XMLHelper::textTag(*doc, DESC_TAG, it.value());
            desc.setAttributeNS(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("xml:lang"),
                                it.key());
            el.appendChild(desc);
        }
    }
    for (const auto &h : d->hashes) {
        auto hashEl = h.toXml(doc);
        if (!hashEl.isNull())
            el.appendChild(hashEl);
    }
    if (!d->mediaType.isEmpty())
        el.appendChild(XMLHelper::textTag(*doc, MEDIA_TYPE_TAG, d->mediaType));
    if (!d->name.isEmpty())
        el.appendChild(XMLHelper::textTag(*doc, NAME_TAG, d->name));
    if (d->size)
        el.appendChild(XMLHelper::textTag(*doc, SIZE_TAG, qint64(*d->size)));
    if (d->thumbnail.isValid())
        el.appendChild(d->thumbnail.toXml(doc));
    if (!jingleExtensions) {
        for (const auto &thumbnail : d->extraThumbnails) {
            if (thumbnail.isValid())
                el.appendChild(thumbnail.toXml(doc));
        }
    }
    if (!jingleExtensions) {
        if (d->width)
            el.appendChild(XMLHelper::textTag(*doc, WIDTH_TAG, qint64(*d->width)));
        if (d->height)
            el.appendChild(XMLHelper::textTag(*doc, HEIGHT_TAG, qint64(*d->height)));
        if (d->length)
            el.appendChild(XMLHelper::textTag(*doc, LENGTH_TAG, qint64(*d->length)));
    }

    if (jingleExtensions) {
        if (d->rangeSupported || d->range.isValid())
            el.appendChild(d->range.toXml(doc));
        if (!d->amplitudes.isEmpty())
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

QDateTime File::date() const { return d ? d->date : QDateTime(); }

QString File::description() const { return d ? d->desc : QString(); }
QString File::description(const QString &language) const
{
    if (!d)
        return {};
    return language.isEmpty() ? d->desc : d->localizedDescriptions.value(language);
}
QMap<QString, QString> File::descriptions() const
{
    if (!d)
        return {};
    auto result = d->localizedDescriptions;
    if (!d->desc.isEmpty())
        result.insert(QString(), d->desc);
    return result;
}

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

std::optional<std::uint64_t> File::size() const { return d ? d->size : std::optional<std::uint64_t> {}; }

Range File::range() const { return d ? d->range : Range(); }

Thumbnail        File::thumbnail() const { return d ? d->thumbnail : Thumbnail(); }
QList<Thumbnail> File::thumbnails() const
{
    if (!d)
        return {};
    auto result = d->extraThumbnails;
    if (d->thumbnail.isValid())
        result.prepend(d->thumbnail);
    return result;
}

QByteArray                   File::amplitudes() const { return d ? d->amplitudes : QByteArray(); }
std::optional<std::uint32_t> File::width() const { return d ? d->width : std::optional<std::uint32_t> {}; }
std::optional<std::uint32_t> File::height() const { return d ? d->height : std::optional<std::uint32_t> {}; }
std::optional<std::uint64_t> File::length() const { return d ? d->length : std::optional<std::uint64_t> {}; }

void File::setDate(const QDateTime &date) { ensureD()->date = date; }

void File::setDescription(const QString &desc) { ensureD()->desc = desc; }
void File::setDescription(const QString &desc, const QString &language)
{
    if (language.isEmpty())
        ensureD()->desc = desc;
    else
        ensureD()->localizedDescriptions.insert(language, desc);
}
void File::setDescriptions(const QMap<QString, QString> &descriptions)
{
    auto p                   = ensureD();
    p->desc                  = descriptions.value(QString());
    p->localizedDescriptions = descriptions;
    p->localizedDescriptions.remove(QString());
}

void File::addHash(const Hash &hash) { ensureD()->hashes.append(hash); }

void File::setHashes(const QList<Hash> &hashes) { ensureD()->hashes = hashes; }

void File::setMediaType(const QString &mediaType) { ensureD()->mediaType = mediaType; }

void File::setName(const QString &name) { ensureD()->name = name; }

void File::setSize(std::uint64_t size) { ensureD()->size = size; }

void File::setRange(const Range &range)
{
    ensureD()->range  = range;
    d->rangeSupported = true;
}

void File::setThumbnail(const Thumbnail &thumb)
{
    auto p       = ensureD();
    p->thumbnail = thumb;
    p->extraThumbnails.clear();
}
void File::setThumbnails(const QList<Thumbnail> &thumbnails)
{
    auto p       = ensureD();
    p->thumbnail = {};
    p->extraThumbnails.clear();
    for (const auto &thumbnail : thumbnails)
        addThumbnail(thumbnail);
}
void File::addThumbnail(const Thumbnail &thumbnail)
{
    if (!thumbnail.isValid())
        return;
    auto p = ensureD();
    if (!p->thumbnail.isValid())
        p->thumbnail = thumbnail;
    else
        p->extraThumbnails.append(thumbnail);
}

void File::setAmplitudes(const QByteArray &amplitudes) { ensureD()->amplitudes = amplitudes; }
void File::setWidth(std::uint32_t width) { ensureD()->width = width; }
void File::setHeight(std::uint32_t height) { ensureD()->height = height; }
void File::setLength(std::uint64_t length) { ensureD()->length = length; }

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