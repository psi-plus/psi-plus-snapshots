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

#ifndef XMPP_JINGLE_FILETRANSFER_FILE_H
#define XMPP_JINGLE_FILETRANSFER_FILE_H

#include "xmpp_hash.h"
#include "xmpp_thumbs.h"

#include <QDateTime>
#include <QObject>

namespace XMPP::Jingle::FileTransfer {
struct Range {
    std::uint64_t offset = 0; // 0 - default value from spec even when not set.
    std::uint64_t length = 0; // 0 - from offset to the end of the file
    QList<Hash>   hashes;

    inline Range() { }
    inline Range(std::uint64_t offset, std::uint64_t length) : offset(offset), length(length) { }
    inline bool isValid() const { return offset || length; }
    inline      operator bool() const { return isValid(); }
    QDomElement toXml(QDomDocument *doc) const;
};

class File {
public:
    File();
    File(const File &other);
    File(const QDomElement &file);
    ~File();
    File       &operator=(const File &other);
    inline bool isValid() const { return d != nullptr; }
    QDomElement toXml(QDomDocument *doc) const;
    bool        merge(const File &other);
    bool        hasComputedHashes() const;

    QDateTime                    date() const;
    QString                      description() const;
    QList<Hash>                  hashes() const;
    QList<Hash>                  computedHashes() const;
    Hash                         hash(Hash::Type t = Hash::Unknown) const;
    QString                      mediaType() const;
    QString                      name() const;
    std::optional<std::uint64_t> size() const;
    Range                        range() const;
    Thumbnail                    thumbnail() const;
    QByteArray                   amplitudes() const;

    void setDate(const QDateTime &date);
    void setDescription(const QString &desc);
    void addHash(const Hash &hash);
    void setHashes(const QList<Hash> &hashes);
    void setMediaType(const QString &mediaType);
    void setName(const QString &name);
    void setSize(uint64_t size);
    void setRange(const Range &range = Range()); // default empty just to indicate it's supported
    void setThumbnail(const Thumbnail &thumb);
    void setAmplitudes(const QByteArray &amplitudes);

private:
    class Private;
    Private                    *ensureD();
    QSharedDataPointer<Private> d;
};

class FileHasher : public QObject {
    Q_OBJECT
public:
    FileHasher(Hash::Type type);
    ~FileHasher();

    /**
     * @brief addData add next portion of data for hash computation.
     * @param data to be added to hash function. if empty it will signal hashing thread to exit
     */
    void addData(const QByteArray &data = QByteArray());
    Hash result();

private:
    class Private;
    std::unique_ptr<Private> d;
};
}

#endif // XMPP_JINGLE_FILETRANSFER_FILE_H
