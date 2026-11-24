/*
 * Copyright (C) 2021-2026  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "xmpp_encryption.h"

#include <QPointer>
#include <QSet>

#include <algorithm>
#include <utility>

namespace XMPP {

namespace {
    QString trustKey(const QString &methodId, const Jid &owner, const QByteArray &keyId)
    {
        return methodId + QLatin1Char('\n') + owner.bare() + QLatin1Char('\n')
            + QString::fromLatin1(keyId.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }
} // namespace

EncryptionTrustLevel MemoryEncryptionTrustStorage::trustLevel(const QString &methodId, const Jid &owner,
                                                              const QByteArray &keyId) const
{
    return levels_.value(trustKey(methodId, owner, keyId), EncryptionTrustLevel::Undecided);
}

bool MemoryEncryptionTrustStorage::setTrustLevel(const QString &methodId, const Jid &owner, const QByteArray &keyId,
                                                 EncryptionTrustLevel level)
{
    if (methodId.isEmpty() || owner.isEmpty() || keyId.isEmpty())
        return false;
    levels_.insert(trustKey(methodId, owner, keyId), level);
    return true;
}

bool MemoryEncryptionTrustStorage::removeTrust(const QString &methodId, const Jid &owner, const QByteArray &keyId)
{
    return levels_.remove(trustKey(methodId, owner, keyId));
}

class EncryptionJob::Private {
public:
    bool               finished = false;
    Error              error    = Error::None;
    QString            errorString;
    QDomDocument       stanzaDocument;
    QByteArray         data;
    EncryptionMetadata metadata;
};

EncryptionJob::EncryptionJob(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) { }
EncryptionJob::~EncryptionJob() = default;

bool                      EncryptionJob::isFinished() const { return d->finished; }
bool                      EncryptionJob::success() const { return d->finished && d->error == Error::None; }
EncryptionJob::Error      EncryptionJob::error() const { return d->error; }
QString                   EncryptionJob::errorString() const { return d->errorString; }
QDomElement               EncryptionJob::stanza() const { return d->stanzaDocument.documentElement(); }
QByteArray                EncryptionJob::data() const { return d->data; }
const EncryptionMetadata &EncryptionJob::metadata() const { return d->metadata; }

void EncryptionJob::complete(const QDomElement &stanza, const EncryptionMetadata &metadata)
{
    if (d->finished)
        return;
    d->stanzaDocument.clear();
    if (!stanza.isNull())
        d->stanzaDocument.appendChild(d->stanzaDocument.importNode(stanza, true));
    d->metadata = metadata;
    d->error    = Error::None;
    d->finished = true;
    emit finished();
}

void EncryptionJob::complete(const QByteArray &data, const EncryptionMetadata &metadata)
{
    if (d->finished)
        return;
    d->data     = data;
    d->metadata = metadata;
    d->error    = Error::None;
    d->finished = true;
    emit finished();
}

void EncryptionJob::fail(Error error, const QString &message, const EncryptionMetadata &metadata)
{
    if (d->finished)
        return;
    d->error       = error == Error::None ? Error::ProtocolError : error;
    d->errorString = message;
    d->metadata    = metadata;
    d->finished    = true;
    emit finished();
}

class EncryptedSession::Private {
public:
    QString               methodId;
    EncryptionContext     context;
    QSet<EncryptionJob *> activeJobs;
    bool                  closing = false;
    bool                           streamBusy     = false;
    bool                           streamFinished = false;
    std::optional<bool>            streamEncrypting;
    EncryptionMethod::Capabilities requestedCapabilities;
    bool                           capabilitiesConfigured = false;
};

EncryptedSession::EncryptedSession(const QString &methodId, const EncryptionContext &context, QObject *parent) :
    QObject(parent), d(std::make_unique<Private>())
{
    d->methodId = methodId;
    d->context  = context;
}

EncryptedSession::~EncryptedSession() = default;

QString EncryptedSession::methodId() const { return d->methodId; }

const EncryptionContext &EncryptedSession::context() const { return d->context; }

bool EncryptedSession::isClosing() const { return d->closing; }
bool EncryptedSession::hasActiveOperations() const { return !d->activeJobs.isEmpty(); }

void EncryptedSession::close()
{
    d->closing = true;
    if (d->activeJobs.isEmpty())
        deleteLater();
}

EncryptionJob *EncryptedSession::track(EncryptionJob *job)
{
    if (!job) {
        auto *failed = new EncryptionJob(parent() ? parent() : this);
        failed->fail(EncryptionJob::Error::ProtocolError,
                     QStringLiteral("Encryption session did not return an operation"));
        return failed;
    }
    // Jobs may outlive a completed transient session. Keep results with the owning
    // factory when one exists, rather than destroying them with close().
    job->setParent(parent() ? parent() : this);
    d->activeJobs.insert(job);
    connect(job, &EncryptionJob::finished, this, [this, job]() { operationFinished(job); });
    connect(job, &QObject::destroyed, this, [this, job]() { operationFinished(job); });
    if (job->isFinished())
        operationFinished(job);
    return job;
}

void EncryptedSession::operationFinished(EncryptionJob *job)
{
    if (!d->activeJobs.remove(job))
        return;
    if (d->closing && d->activeJobs.isEmpty())
        deleteLater();
}

static EncryptionJob *unsupportedJob(QObject *parent)
{
    auto job = new EncryptionJob(parent);
    job->fail(EncryptionJob::Error::Unsupported,
              QStringLiteral("Operation is not supported by this encryption implementation"));
    return job;
}

EncryptionJob *EncryptedSession::doEncrypt(const QDomElement &, const EncryptionContext &)
{
    return unsupportedJob(this);
}
EncryptionJob *EncryptedSession::doDecrypt(const QDomElement &, const EncryptionContext &)
{
    return unsupportedJob(this);
}
EncryptionJob *EncryptedSession::doEncrypt(const QByteArray &) { return unsupportedJob(this); }
EncryptionJob *EncryptedSession::doDecrypt(const QByteArray &) { return unsupportedJob(this); }
EncryptionJob *EncryptedSession::doEncryptChunk(const QByteArray &, bool) { return unsupportedJob(this); }
EncryptionJob *EncryptedSession::doDecryptChunk(const QByteArray &, bool) { return unsupportedJob(this); }

// Immutable per-call context avoids races between overlapping async stanzas.
// The bound peer is the default recipient; a stanza may override it (e.g. a
// group message with multiple recipients or a reply to an encrypted request).
static EncryptionContext effectiveContext(const EncryptionContext &session, const EncryptionContext &operation)
{
    EncryptionContext result = session;
    if (!operation.recipients.isEmpty())
        result.recipients = operation.recipients;
    if (operation.replyTo)
        result.replyTo = operation.replyTo;
    for (auto it = operation.options.cbegin(); it != operation.options.cend(); ++it)
        result.options.insert(it.key(), it.value());
    return result;
}

EncryptionJob *EncryptedSession::encrypt(const QDomElement &xml, const EncryptionContext &operation)
{
    if (d->closing || (d->capabilitiesConfigured && !d->requestedCapabilities.testFlag(EncryptionMethod::XmppStanza)))
        return unsupportedJob(parent() ? parent() : this);
    return track(doEncrypt(xml, effectiveContext(d->context, operation)));
}

EncryptionJob *EncryptedSession::decrypt(const QDomElement &xml, const EncryptionContext &operation)
{
    if (d->closing || (d->capabilitiesConfigured && !d->requestedCapabilities.testFlag(EncryptionMethod::XmppStanza)))
        return unsupportedJob(parent() ? parent() : this);
    return track(doDecrypt(xml, effectiveContext(d->context, operation)));
}

EncryptionJob *EncryptedSession::encrypt(const QByteArray &data)
{
    if (d->closing || (d->capabilitiesConfigured && !d->requestedCapabilities.testFlag(EncryptionMethod::DataMessage)))
        return unsupportedJob(parent() ? parent() : this);
    return track(doEncrypt(data));
}

EncryptionJob *EncryptedSession::decrypt(const QByteArray &data)
{
    if (d->closing || (d->capabilitiesConfigured && !d->requestedCapabilities.testFlag(EncryptionMethod::DataMessage)))
        return unsupportedJob(parent() ? parent() : this);
    return track(doDecrypt(data));
}

EncryptionJob *EncryptedSession::encryptChunk(const QByteArray &chunk, bool final)
{
    return runStream(chunk, true, final);
}

EncryptionJob *EncryptedSession::decryptChunk(const QByteArray &chunk, bool final)
{
    return runStream(chunk, false, final);
}

EncryptionJob *EncryptedSession::runStream(const QByteArray &chunk, bool encrypting, bool final)
{
    if (d->closing || (d->capabilitiesConfigured && !d->requestedCapabilities.testFlag(EncryptionMethod::DataStream))
        || d->streamBusy || d->streamFinished)
        return unsupportedJob(parent() ? parent() : this);
    if (d->streamEncrypting && *d->streamEncrypting != encrypting)
        return unsupportedJob(parent() ? parent() : this);

    d->streamBusy       = true;
    d->streamEncrypting = encrypting;
    auto *job           = encrypting ? doEncryptChunk(chunk, final) : doDecryptChunk(chunk, final);
    if (!job) {
        d->streamBusy     = false;
        d->streamFinished = true;
        return track(nullptr);
    }
    const auto finish = [this, jobGuard = QPointer<EncryptionJob>(job), final]() {
        d->streamBusy = false;
        if (final || !jobGuard || !jobGuard->success())
            d->streamFinished = true;
    };
    if (job->isFinished())
        finish();
    else
        connect(job, &EncryptionJob::finished, this, finish);
    return track(job);
}

EncryptionMethod::EncryptionMethod(QObject *parent) : QObject(parent) { }
EncryptionMethod::~EncryptionMethod() = default;

EncryptionMethod::Availability EncryptionMethod::availability(const Jid &) const { return Availability::Available; }

EncryptionJob *EncryptionMethod::prepareDecryptionRecovery(const EncryptionMetadata &) { return unsupportedJob(this); }

EncryptionJob *EncryptionMethod::recoverDecryption(const EncryptionMetadata &) { return unsupportedJob(this); }

class EncryptionManager::Private {
public:
    QList<QPointer<EncryptionMethod>> methods;
};

EncryptionManager::EncryptionManager(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) { }
EncryptionManager::~EncryptionManager() = default;

bool EncryptionManager::registerMethod(EncryptionMethod *method)
{
    if (!method || method->id().isEmpty() || this->method(method->id()))
        return false;
    const QString methodId = method->id();
    d->methods.append(method);
    connect(method, &QObject::destroyed, this, [this, method, methodId]() {
        const auto oldSize = d->methods.size();
        d->methods.erase(
            std::remove_if(d->methods.begin(), d->methods.end(),
                           [method](const auto &entry) { return entry.isNull() || entry.data() == method; }),
            d->methods.end());
        if (d->methods.size() != oldSize) {
            for (auto *session : findChildren<EncryptedSession *>(QString(), Qt::FindDirectChildrenOnly)) {
                if (session && session->methodId() == methodId)
                    session->close();
            }
            emit methodUnregistered(methodId);
        }
    });
    emit methodRegistered(methodId);
    return true;
}

void EncryptionManager::unregisterMethod(EncryptionMethod *method)
{
    if (!method)
        return;
    const QString id      = method->id();
    const auto    oldSize = d->methods.size();
    d->methods.erase(std::remove_if(d->methods.begin(), d->methods.end(),
                                    [method](const auto &entry) { return entry.isNull() || entry == method; }),
                     d->methods.end());
    if (d->methods.size() != oldSize) {
        for (auto *session : findChildren<EncryptedSession *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (session && session->methodId() == id)
                session->close();
        }
        emit methodUnregistered(id);
    }
}

EncryptionMethod *EncryptionManager::method(const QString &id) const
{
    for (const auto &entry : std::as_const(d->methods)) {
        if (entry && entry->id() == id)
            return entry;
    }
    return nullptr;
}

EncryptionMethod *EncryptionManager::methodForStanza(const QDomElement &stanza) const
{
    for (const auto &entry : std::as_const(d->methods)) {
        if (entry && entry->capabilities().testFlag(EncryptionMethod::XmppStanza) && entry->canDecrypt(stanza))
            return entry;
    }
    return nullptr;
}

EncryptionMethod::Availability EncryptionManager::availability(const QString &methodId, const Jid &peer) const
{
    auto *entry = method(methodId);
    return entry ? entry->availability(peer) : EncryptionMethod::Availability::Unavailable;
}

EncryptionManager::MethodsMap EncryptionManager::methods(EncryptionMethod::Capabilities caps) const
{
    MethodsMap ret;
    for (const auto &entry : std::as_const(d->methods)) {
        if (!entry)
            continue;
        if (caps == EncryptionMethod::Capabilities() || (entry->capabilities() & caps) == caps)
            ret[entry->id()] = entry->name();
    }
    return ret;
}

Features EncryptionManager::features() const
{
    Features result;
    for (const auto &entry : std::as_const(d->methods)) {
        if (entry)
            result += entry->features();
    }
    return result;
}

EncryptedSession *EncryptionManager::startSession(const QString &methodId, EncryptionMethod::Capabilities capabilities,
                                                  const EncryptionContext &context)
{
    auto *method = this->method(methodId);
    if (!method || capabilities == EncryptionMethod::Capabilities()
        || (method->capabilities() & capabilities) != capabilities)
        return nullptr;

    auto *session = method->startSession(capabilities, context);
    if (!session)
        return nullptr;
    if (session->methodId() != methodId) {
        delete session;
        return nullptr;
    }
    session->setParent(this);
    session->d->requestedCapabilities  = capabilities;
    session->d->capabilitiesConfigured = true;
    return session;
}

} // namespace XMPP
