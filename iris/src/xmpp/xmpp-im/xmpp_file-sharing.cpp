/*
 * xmpp_file-sharing.cpp - XEP-0447 / XEP-0448 stateless file sharing
 * Copyright (C) 2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "xmpp_file-sharing.h"

#include <QtCrypto>

#include <algorithm>
#include <cstring>
#include <limits>

namespace XMPP::StatelessFileSharing {

const QString NS                   = QStringLiteral("urn:xmpp:sfs:0");
const QString ENCRYPTED_NS         = QStringLiteral("urn:xmpp:esfs:0");
const QString URL_DATA_NS          = QStringLiteral("http://jabber.org/protocol/url-data");
const QString MESSAGE_ATTACHING_NS = QStringLiteral("urn:xmpp:message-attaching:1");

static constexpr qsizetype GcmTagSize = 16;

QString cipherUri(Cipher cipher)
{
    switch (cipher) {
    case Cipher::Aes128Gcm:
        return QStringLiteral("urn:xmpp:ciphers:aes-128-gcm-nopadding:0");
    case Cipher::Aes256Gcm:
        return QStringLiteral("urn:xmpp:ciphers:aes-256-gcm-nopadding:0");
    case Cipher::Aes256CbcPkcs7:
        return QStringLiteral("urn:xmpp:ciphers:aes-256-cbc-pkcs7:0");
    default:
        return {};
    }
}

Cipher cipherFromUri(const QString &uri)
{
    if (uri == QLatin1String("urn:xmpp:ciphers:aes-128-gcm-nopadding:0"))
        return Cipher::Aes128Gcm;
    if (uri == QLatin1String("urn:xmpp:ciphers:aes-256-gcm-nopadding:0"))
        return Cipher::Aes256Gcm;
    if (uri == QLatin1String("urn:xmpp:ciphers:aes-256-cbc-pkcs7:0"))
        return Cipher::Aes256CbcPkcs7;
    return Cipher::Unknown;
}

static qsizetype keySize(Cipher cipher)
{
    switch (cipher) {
    case Cipher::Aes128Gcm:
        return 16;
    case Cipher::Aes256Gcm:
    case Cipher::Aes256CbcPkcs7:
        return 32;
    default:
        return 0;
    }
}

static qsizetype ivSize(Cipher cipher)
{
    switch (cipher) {
    case Cipher::Aes128Gcm:
    case Cipher::Aes256Gcm:
        return 12;
    case Cipher::Aes256CbcPkcs7:
        return 16;
    default:
        return 0;
    }
}

static QString qcaAlgorithm(Cipher cipher)
{
    return cipher == Cipher::Aes128Gcm ? QStringLiteral("aes128") : QStringLiteral("aes256");
}

bool cipherSupported(Cipher cipher)
{
    if (cipher == Cipher::Unknown)
        return false;
    if (cipher == Cipher::Aes256CbcPkcs7)
        return QCA::Cipher::supportedTypes().contains(
            QCA::Cipher::withAlgorithms(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7));
    return QCA::Cipher::supportedTypes().contains(
        QCA::Cipher::withAlgorithms(qcaAlgorithm(cipher), QCA::Cipher::GCM, QCA::Cipher::NoPadding));
}

// Source ---------------------------------------------------------------------
Source::Source() = default;
Source::Source(const QDomElement &element) { fromXml(element); }

Source Source::fromUrl(const QUrl &url)
{
    Source source;
    if (url.isValid() && !url.isEmpty()) {
        source.type_ = Type::UrlData;
        source.url_  = url;
    }
    return source;
}

Source Source::fromJinglePub(const Jingle::JinglePub &publication)
{
    Source source;
    if (publication.isValid()) {
        source.type_      = Type::JinglePub;
        source.jinglePub_ = publication;
    }
    return source;
}

Source Source::fromEncrypted(const EncryptedSource &encrypted)
{
    Source source;
    if (encrypted.isValid()) {
        source.type_      = Type::Encrypted;
        source.encrypted_ = QSharedPointer<EncryptedSource>::create(encrypted);
    }
    return source;
}

Source Source::fromElement(const QDomElement &element)
{
    Source source;
    source.fromXml(element);
    return source;
}

bool Source::isValid() const
{
    switch (type_) {
    case Type::UrlData:
        return url_.isValid() && !url_.isEmpty();
    case Type::JinglePub:
        return jinglePub_.isValid();
    case Type::Encrypted:
        return encrypted_ && encrypted_->isValid();
    case Type::Other:
        return !rawElement_.isNull() && !rawElement_.namespaceURI().isEmpty();
    default:
        return false;
    }
}

Source::Type      Source::type() const { return type_; }
QUrl              Source::url() const { return type_ == Type::UrlData ? url_ : QUrl(); }
Jingle::JinglePub Source::jinglePub() const { return type_ == Type::JinglePub ? jinglePub_ : Jingle::JinglePub(); }
EncryptedSource   Source::encrypted() const
{
    return type_ == Type::Encrypted && encrypted_ ? *encrypted_ : EncryptedSource();
}
QDomElement Source::rawElement() const { return rawElement_; }

bool Source::fromXml(const QDomElement &element)
{
    type_ = Type::Invalid;
    url_.clear();
    jinglePub_ = {};
    encrypted_.clear();
    rawElement_ = {};

    if (element.localName() == QLatin1String("url-data") && element.namespaceURI() == URL_DATA_NS) {
        const auto url = QUrl(element.attribute(QStringLiteral("target")), QUrl::StrictMode);
        if (!url.isValid() || url.isEmpty())
            return false;
        type_       = Type::UrlData;
        url_        = url;
        rawElement_ = element; // preserve optional XEP-0104 HTTP request information
        return true;
    }
    if (element.localName() == QLatin1String("jinglepub") && element.namespaceURI() == Jingle::JINGLEPUB_NS) {
        Jingle::JinglePub publication(element);
        if (!publication.isValid())
            return false;
        type_      = Type::JinglePub;
        jinglePub_ = publication;
        return true;
    }
    if (element.localName() == QLatin1String("encrypted") && element.namespaceURI() == ENCRYPTED_NS) {
        EncryptedSource encrypted(element);
        if (!encrypted.isValid())
            return false;
        type_      = Type::Encrypted;
        encrypted_ = QSharedPointer<EncryptedSource>::create(encrypted);
        return true;
    }
    if (!element.isNull() && !element.namespaceURI().isEmpty()) {
        type_       = Type::Other;
        rawElement_ = element;
        return true;
    }
    return false;
}

QDomElement Source::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};
    switch (type_) {
    case Type::UrlData: {
        if (!rawElement_.isNull()) {
            auto element = doc->importNode(rawElement_, true).toElement();
            element.setAttribute(QStringLiteral("target"), url_.toString(QUrl::FullyEncoded));
            return element;
        }
        auto element = doc->createElementNS(URL_DATA_NS, QStringLiteral("url-data"));
        element.setAttribute(QStringLiteral("target"), url_.toString(QUrl::FullyEncoded));
        return element;
    }
    case Type::JinglePub:
        return jinglePub_.toXml(doc);
    case Type::Encrypted:
        return encrypted_->toXml(doc);
    case Type::Other:
        return doc->importNode(rawElement_, true).toElement();
    default:
        return {};
    }
}

// Sources --------------------------------------------------------------------
Sources::Sources() = default;
Sources::Sources(const QDomElement &element) { fromXml(element); }
bool          Sources::isValid() const { return !items_.isEmpty(); }
bool          Sources::isEmpty() const { return items_.isEmpty(); }
QString       Sources::id() const { return id_; }
void          Sources::setId(const QString &id) { id_ = id; }
QList<Source> Sources::items() const { return items_; }
void          Sources::setItems(const QList<Source> &items) { items_ = items; }
void          Sources::add(const Source &source)
{
    if (source.isValid())
        items_.append(source);
}

bool Sources::fromXml(const QDomElement &element)
{
    id_.clear();
    items_.clear();
    if (element.localName() != QLatin1String("sources") || element.namespaceURI() != NS)
        return false;
    id_ = element.attribute(QStringLiteral("id"));
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        Source source(child);
        if (source.isValid())
            items_.append(source);
    }
    return !items_.isEmpty();
}

QDomElement Sources::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};
    auto root = doc->createElementNS(NS, QStringLiteral("sources"));
    if (!id_.isEmpty())
        root.setAttribute(QStringLiteral("id"), id_);
    for (const auto &source : items_) {
        auto element = source.toXml(doc);
        if (!element.isNull())
            root.appendChild(element);
    }
    return root;
}

// EncryptedSource ------------------------------------------------------------
EncryptedSource::EncryptedSource() = default;
EncryptedSource::EncryptedSource(const QDomElement &element) { fromXml(element); }

bool EncryptedSource::isValid() const
{
    return cipher_ != Cipher::Unknown && key_.size() == keySize(cipher_) && iv_.size() == ivSize(cipher_)
        && sources_.isValid();
}
Cipher      EncryptedSource::cipher() const { return cipher_; }
void        EncryptedSource::setCipher(Cipher cipher) { cipher_ = cipher; }
QByteArray  EncryptedSource::key() const { return key_; }
void        EncryptedSource::setKey(const QByteArray &key) { key_ = key; }
QByteArray  EncryptedSource::iv() const { return iv_; }
void        EncryptedSource::setIv(const QByteArray &iv) { iv_ = iv; }
QList<Hash> EncryptedSource::hashes() const { return hashes_; }
void        EncryptedSource::setHashes(const QList<Hash> &hashes) { hashes_ = hashes; }
void        EncryptedSource::addHash(const Hash &hash)
{
    if (hash.isValid())
        hashes_.append(hash);
}
Sources EncryptedSource::sources() const { return sources_; }
void    EncryptedSource::setSources(const Sources &sources) { sources_ = sources; }

bool EncryptedSource::fromXml(const QDomElement &element)
{
    cipher_ = Cipher::Unknown;
    key_.clear();
    iv_.clear();
    hashes_.clear();
    sources_ = {};
    if (element.localName() != QLatin1String("encrypted") || element.namespaceURI() != ENCRYPTED_NS)
        return false;

    cipher_ = cipherFromUri(element.attribute(QStringLiteral("cipher")));
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI() == ENCRYPTED_NS && child.localName() == QLatin1String("key")) {
            key_ = QByteArray::fromBase64(child.text().toLatin1());
        } else if (child.namespaceURI() == ENCRYPTED_NS && child.localName() == QLatin1String("iv")) {
            iv_ = QByteArray::fromBase64(child.text().toLatin1());
        } else if (child.namespaceURI() == HASH_NS && child.localName() == QLatin1String("hash")) {
            Hash hash(child);
            if (hash.isValid())
                hashes_.append(hash);
        } else if (child.namespaceURI() == NS && child.localName() == QLatin1String("sources")) {
            sources_.fromXml(child);
        }
    }
    return isValid();
}

QDomElement EncryptedSource::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};
    auto root = doc->createElementNS(ENCRYPTED_NS, QStringLiteral("encrypted"));
    root.setAttribute(QStringLiteral("cipher"), cipherUri(cipher_));

    auto key = doc->createElementNS(ENCRYPTED_NS, QStringLiteral("key"));
    key.appendChild(doc->createTextNode(QString::fromLatin1(key_.toBase64())));
    root.appendChild(key);
    auto iv = doc->createElementNS(ENCRYPTED_NS, QStringLiteral("iv"));
    iv.appendChild(doc->createTextNode(QString::fromLatin1(iv_.toBase64())));
    root.appendChild(iv);
    for (const auto &hash : hashes_) {
        auto hashEl = hash.toXml(doc);
        if (!hashEl.isNull())
            root.appendChild(hashEl);
    }
    root.appendChild(sources_.toXml(doc));
    return root;
}

// FileSharing ----------------------------------------------------------------
FileSharing::FileSharing() = default;
FileSharing::FileSharing(const QDomElement &element) { fromXml(element); }
bool FileSharing::isValid() const
{
    if (!file_.isValid())
        return false;
    const auto sourceItems        = sources_.items();
    const bool hasEncryptedSource = std::any_of(sourceItems.cbegin(), sourceItems.cend(), [](const Source &source) {
        return source.type() == Source::Type::Encrypted;
    });
    if (hasEncryptedSource && (!file_.size().has_value() || file_.computedHashes().isEmpty()))
        return false;
    return true;
}
Disposition                FileSharing::disposition() const { return disposition_; }
void                       FileSharing::setDisposition(Disposition disposition) { disposition_ = disposition; }
QString                    FileSharing::id() const { return id_; }
void                       FileSharing::setId(const QString &id) { id_ = id; }
Jingle::FileTransfer::File FileSharing::file() const { return file_; }
void                       FileSharing::setFile(const Jingle::FileTransfer::File &file) { file_ = file; }
Sources                    FileSharing::sources() const { return sources_; }
void                       FileSharing::setSources(const Sources &sources) { sources_ = sources; }

bool FileSharing::fromXml(const QDomElement &element)
{
    disposition_ = Disposition::Unspecified;
    id_.clear();
    file_    = {};
    sources_ = {};
    if (element.localName() != QLatin1String("file-sharing") || element.namespaceURI() != NS)
        return false;

    const auto disposition = element.attribute(QStringLiteral("disposition"));
    if (disposition == QLatin1String("inline"))
        disposition_ = Disposition::Inline;
    else if (disposition == QLatin1String("attachment"))
        disposition_ = Disposition::Attachment;
    id_ = element.attribute(QStringLiteral("id"));

    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.localName() == QLatin1String("file")
            && child.namespaceURI() == Jingle::FileTransfer::FILE_METADATA_NS)
            file_ = Jingle::FileTransfer::File(child);
        else if (child.localName() == QLatin1String("sources") && child.namespaceURI() == NS)
            sources_.fromXml(child);
    }
    return isValid();
}

QDomElement FileSharing::toXml(QDomDocument *doc) const
{
    if (!doc || !isValid())
        return {};
    auto root = doc->createElementNS(NS, QStringLiteral("file-sharing"));
    if (disposition_ == Disposition::Inline)
        root.setAttribute(QStringLiteral("disposition"), QStringLiteral("inline"));
    else if (disposition_ == Disposition::Attachment)
        root.setAttribute(QStringLiteral("disposition"), QStringLiteral("attachment"));
    if (!id_.isEmpty())
        root.setAttribute(QStringLiteral("id"), id_);
    root.appendChild(file_.toMetadataXml(doc));
    if (sources_.isValid())
        root.appendChild(sources_.toXml(doc));
    return root;
}

// Crypto ---------------------------------------------------------------------
std::optional<std::uint64_t> encryptedSize(Cipher cipher, std::uint64_t plaintextSize)
{
    switch (cipher) {
    case Cipher::Aes128Gcm:
    case Cipher::Aes256Gcm:
        if (plaintextSize > std::numeric_limits<std::uint64_t>::max() - std::uint64_t(GcmTagSize))
            return std::nullopt;
        return plaintextSize + std::uint64_t(GcmTagSize);
    case Cipher::Aes256CbcPkcs7: {
        constexpr std::uint64_t BlockSize = 16;
        if (plaintextSize > std::numeric_limits<std::uint64_t>::max() - BlockSize)
            return std::nullopt;
        return (plaintextSize / BlockSize + 1) * BlockSize;
    }
    default:
        return std::nullopt;
    }
}

class EncryptingDevice::Private {
public:
    QIODevice                   *source = nullptr;
    Cipher                       cipher = Cipher::Unknown;
    QByteArray                   key;
    QByteArray                   iv;
    std::unique_ptr<QCA::Cipher> context;
    std::unique_ptr<StreamHash>  hasher;
    QByteArray                   pending;
    Hash                         hash;
    bool                         valid    = false;
    bool                         finished = false;
    bool                         failed   = false;

    Private(QIODevice *source, Cipher cipher, QByteArray key, QByteArray iv) :
        source(source), cipher(cipher), key(std::move(key)), iv(std::move(iv))
    {
        valid = source && cipherSupported(cipher) && this->key.size() == keySize(cipher)
            && this->iv.size() == ivSize(cipher);
    }

    bool start()
    {
        pending.clear();
        hash     = {};
        finished = false;
        failed   = false;
        hasher   = std::make_unique<StreamHash>(Hash::Sha256);
        if (cipher == Cipher::Aes256CbcPkcs7) {
            context = std::make_unique<QCA::Cipher>(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7,
                                                    QCA::Encode, QCA::SymmetricKey(key), QCA::InitializationVector(iv));
        } else {
            context = std::make_unique<QCA::Cipher>(qcaAlgorithm(cipher), QCA::Cipher::GCM, QCA::Cipher::NoPadding,
                                                    QCA::Encode, QCA::SymmetricKey(key), QCA::InitializationVector(iv),
                                                    QCA::AuthTag(QByteArray(GcmTagSize, '\0')));
        }
        return bool(context);
    }

    bool appendOutput(const QByteArray &data)
    {
        if (data.isEmpty())
            return true;
        if (!hasher || !hasher->addData(data))
            return false;
        pending.append(data);
        return true;
    }

    bool finish()
    {
        if (finished)
            return !failed;
        if (!context || !appendOutput(context->final().toByteArray()) || !context->ok()) {
            failed = true;
            return false;
        }
        if (cipher == Cipher::Aes128Gcm || cipher == Cipher::Aes256Gcm) {
            const auto tag = context->tag().toByteArray();
            if (tag.size() != GcmTagSize || !appendOutput(tag)) {
                failed = true;
                return false;
            }
        }
        hash = hasher ? hasher->final() : Hash();
        if (!hash.isValid() || hash.data().isEmpty()) {
            failed = true;
            return false;
        }
        finished = true;
        return true;
    }
};

EncryptingDevice::EncryptingDevice(QIODevice *source, Cipher cipher, QObject *parent) :
    EncryptingDevice(source, cipher, QCA::Random::randomArray(int(keySize(cipher))).toByteArray(),
                     QCA::Random::randomArray(int(ivSize(cipher))).toByteArray(), parent)
{
}

EncryptingDevice::EncryptingDevice(QIODevice *source, Cipher cipher, const QByteArray &key, const QByteArray &iv,
                                   QObject *parent) :
    QIODevice(parent), d(std::make_unique<Private>(source, cipher, key, iv))
{
    if (source) {
        QObject::connect(source, &QIODevice::readyRead, this, &QIODevice::readyRead);
        QObject::connect(source, &QIODevice::readChannelFinished, this, &QIODevice::readyRead);
    }
}

EncryptingDevice::~EncryptingDevice() = default;

bool EncryptingDevice::open(OpenMode mode)
{
    if (isOpen())
        return false;
    if (mode != QIODevice::ReadOnly || !d->valid || !d->source->isOpen() || !d->source->isReadable()) {
        setErrorString(QStringLiteral("Invalid XEP-0448 encryption source or mode"));
        return false;
    }
    if (!d->start()) {
        setErrorString(QStringLiteral("Failed to initialize XEP-0448 cipher"));
        return false;
    }
    if (!QIODevice::open(mode))
        return false;

    // A finite non-sequential source may already be at EOF (for example an
    // empty QBuffer). Materialize the cipher final output now so consumers
    // can observe it through bytesAvailable() without waiting for readyRead().
    if (!d->source->isSequential() && d->source->atEnd() && !d->finish()) {
        setErrorString(QStringLiteral("Failed to finalize XEP-0448 encryption"));
        QIODevice::close();
        return false;
    }
    return true;
}

void EncryptingDevice::close()
{
    d->context.reset();
    d->hasher.reset();
    d->pending.clear();
    QIODevice::close();
}

bool EncryptingDevice::atEnd() const { return d->finished && d->pending.isEmpty(); }

qint64 EncryptingDevice::bytesAvailable() const
{
    const auto sourceAvailable = d->source ? d->source->bytesAvailable() : qint64(0);
    return qint64(d->pending.size()) + sourceAvailable + QIODevice::bytesAvailable();
}

bool       EncryptingDevice::isValid() const { return d->valid; }
bool       EncryptingDevice::finished() const { return d->finished && !d->failed; }
Cipher     EncryptingDevice::cipher() const { return d->cipher; }
QByteArray EncryptingDevice::key() const { return d->key; }
QByteArray EncryptingDevice::iv() const { return d->iv; }
Hash       EncryptingDevice::encryptedHash() const { return finished() ? d->hash : Hash(); }

qint64 EncryptingDevice::readData(char *data, qint64 maxSize)
{
    if (!data || maxSize <= 0 || !isOpen() || d->failed)
        return d->failed ? -1 : 0;

    constexpr qint64 ChunkSize = 64 * 1024;
    while (d->pending.isEmpty() && !d->finished) {
        const auto input = d->source->read(ChunkSize);
        if (!input.isEmpty()) {
            const auto output = d->context->update(QCA::MemoryRegion(input)).toByteArray();
            if (!d->context->ok() || !d->appendOutput(output)) {
                d->failed = true;
                setErrorString(QStringLiteral("XEP-0448 encryption failed"));
                return -1;
            }
            // QFile/QBuffer-style sources do not emit another readyRead after
            // their last bytes have been consumed. Finalize in the same read
            // so the GCM tag/CBC padding is already part of pending output.
            // Otherwise an asynchronous consumer such as QNetworkAccessManager
            // can drain the ciphertext, see bytesAvailable() == 0 while
            // atEnd() is still false, and wait forever for a signal.
            if (!d->source->isSequential() && d->source->atEnd() && !d->finish()) {
                setErrorString(QStringLiteral("Failed to finalize XEP-0448 encryption"));
                return -1;
            }
            continue;
        }
        if (!d->source->atEnd())
            return 0;
        if (!d->finish()) {
            setErrorString(QStringLiteral("Failed to finalize XEP-0448 encryption"));
            return -1;
        }
    }

    if (d->pending.isEmpty())
        return 0;
    const auto count = qMin<qint64>(maxSize, d->pending.size());
#ifdef XMPP_DEBUG
    qDebug() << "EncryptingDevice::readData " << count << "bytes";
#endif
    std::memcpy(data, d->pending.constData(), std::size_t(count));
    d->pending.remove(0, qsizetype(count));
    return count;
}

qint64 EncryptingDevice::writeData(const char *, qint64)
{
    setErrorString(QStringLiteral("XEP-0448 EncryptingDevice is read-only"));
    return -1;
}

std::optional<EncryptedPayload> encrypt(Cipher cipher, const QByteArray &plaintext)
{
    if (!cipherSupported(cipher))
        return std::nullopt;

    EncryptedPayload result;
    result.key = QCA::Random::randomArray(int(keySize(cipher))).toByteArray();
    result.iv  = QCA::Random::randomArray(int(ivSize(cipher))).toByteArray();

    if (cipher == Cipher::Aes256CbcPkcs7) {
        QCA::Cipher qcaCipher(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7, QCA::Encode,
                              QCA::SymmetricKey(result.key), QCA::InitializationVector(result.iv));
        auto        encrypted = qcaCipher.process(QCA::MemoryRegion(plaintext));
        if (!qcaCipher.ok())
            return std::nullopt;
        result.data = encrypted.toByteArray();
        return result;
    }

    QCA::Cipher qcaCipher(qcaAlgorithm(cipher), QCA::Cipher::GCM, QCA::Cipher::NoPadding, QCA::Encode,
                          QCA::SymmetricKey(result.key), QCA::InitializationVector(result.iv),
                          QCA::AuthTag(QByteArray(GcmTagSize, '\0')));
    auto        encrypted = qcaCipher.process(QCA::MemoryRegion(plaintext));
    if (!qcaCipher.ok())
        return std::nullopt;
    result.data = encrypted.toByteArray() + qcaCipher.tag().toByteArray();
    if (result.data.size() < GcmTagSize)
        return std::nullopt;
    return result;
}

bool decryptToDevice(Cipher cipher, QIODevice *ciphertext, QIODevice *plaintext, const QByteArray &key,
                     const QByteArray &iv, std::optional<std::uint64_t> originalSize)
{
    if (!ciphertext || !plaintext || !ciphertext->isOpen() || !ciphertext->isReadable() || !plaintext->isOpen()
        || !plaintext->isWritable() || !cipherSupported(cipher) || key.size() != keySize(cipher)
        || iv.size() != ivSize(cipher)) {
        return false;
    }

    QByteArray tag;
    qint64     payloadSize = -1;
    if (cipher == Cipher::Aes128Gcm || cipher == Cipher::Aes256Gcm) {
        if (ciphertext->isSequential() || ciphertext->size() < GcmTagSize)
            return false;
        payloadSize = ciphertext->size() - GcmTagSize;
        if (!ciphertext->seek(payloadSize))
            return false;
        tag = ciphertext->read(GcmTagSize);
        if (tag.size() != GcmTagSize || !ciphertext->seek(0))
            return false;
    }

    std::unique_ptr<QCA::Cipher> context;
    if (cipher == Cipher::Aes256CbcPkcs7) {
        context = std::make_unique<QCA::Cipher>(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7,
                                                QCA::Decode, QCA::SymmetricKey(key), QCA::InitializationVector(iv));
    } else {
        context
            = std::make_unique<QCA::Cipher>(qcaAlgorithm(cipher), QCA::Cipher::GCM, QCA::Cipher::NoPadding, QCA::Decode,
                                            QCA::SymmetricKey(key), QCA::InitializationVector(iv), QCA::AuthTag(tag));
    }

    constexpr qint64 ChunkSize  = 64 * 1024;
    std::uint64_t    written    = 0;
    auto             writePlain = [&](const QByteArray &data) {
        qsizetype count = data.size();
        if (originalSize) {
            if (written >= *originalSize)
                count = 0;
            else
                count = qsizetype(std::min<std::uint64_t>(std::uint64_t(count), *originalSize - written));
        }
        if (count > 0 && plaintext->write(data.constData(), count) != count)
            return false;
        written += std::uint64_t(count);
        return true;
    };

    qint64 remaining = payloadSize;
    while (true) {
        const qint64 requested = remaining >= 0 ? qMin(remaining, ChunkSize) : ChunkSize;
        if (requested == 0)
            break;
        const auto input = ciphertext->read(requested);
        if (input.isEmpty()) {
            if (ciphertext->atEnd())
                break;
            return false;
        }
        if (remaining >= 0)
            remaining -= input.size();
        const auto output = context->update(QCA::MemoryRegion(input)).toByteArray();
        if (!context->ok() || !writePlain(output))
            return false;
    }
    if (remaining > 0)
        return false;
    const auto final = context->final().toByteArray();
    if (!context->ok() || !writePlain(final))
        return false;
    return !originalSize || written == *originalSize;
}

std::optional<QByteArray> decrypt(Cipher cipher, const QByteArray &ciphertext, const QByteArray &key,
                                  const QByteArray &iv, std::optional<std::uint64_t> originalSize)
{
    if (!cipherSupported(cipher) || key.size() != keySize(cipher) || iv.size() != ivSize(cipher))
        return std::nullopt;

    QByteArray plaintext;
    if (cipher == Cipher::Aes256CbcPkcs7) {
        QCA::Cipher qcaCipher(QStringLiteral("aes256"), QCA::Cipher::CBC, QCA::Cipher::PKCS7, QCA::Decode,
                              QCA::SymmetricKey(key), QCA::InitializationVector(iv));
        auto        decrypted = qcaCipher.process(QCA::MemoryRegion(ciphertext));
        if (!qcaCipher.ok())
            return std::nullopt;
        plaintext = decrypted.toByteArray();
    } else {
        if (ciphertext.size() < GcmTagSize)
            return std::nullopt;
        const auto  payload = ciphertext.left(ciphertext.size() - GcmTagSize);
        const auto  tag     = ciphertext.right(GcmTagSize);
        QCA::Cipher qcaCipher(qcaAlgorithm(cipher), QCA::Cipher::GCM, QCA::Cipher::NoPadding, QCA::Decode,
                              QCA::SymmetricKey(key), QCA::InitializationVector(iv), QCA::AuthTag(tag));
        auto        decrypted = qcaCipher.process(QCA::MemoryRegion(payload));
        if (!qcaCipher.ok())
            return std::nullopt;
        plaintext = decrypted.toByteArray();
    }

    if (originalSize) {
        if (*originalSize > std::uint64_t(plaintext.size()))
            return std::nullopt;
        plaintext.truncate(qsizetype(*originalSize));
    }
    return plaintext;
}

} // namespace XMPP::StatelessFileSharing
