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
    return levels_.remove(trustKey(methodId, owner, keyId)) > 0;
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

void EncryptionJob::fail(Error error, const QString &message)
{
    if (d->finished)
        return;
    d->error       = error == Error::None ? Error::ProtocolError : error;
    d->errorString = message;
    d->finished    = true;
    emit finished();
}

class EncryptedSession::Private {
public:
    QString               methodId;
    EncryptionContext     context;
    QSet<EncryptionJob *> activeJobs;
    bool                  closing = false;
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

void EncryptedSession::close()
{
    d->closing = true;
    if (d->activeJobs.isEmpty())
        deleteLater();
}

void EncryptedSession::track(EncryptionJob *job)
{
    if (!job)
        return;
    d->activeJobs.insert(job);
    connect(job, &EncryptionJob::finished, this, [this, job]() { operationFinished(job); });
    connect(job, &QObject::destroyed, this, [this, job]() { operationFinished(job); });
    if (job->isFinished())
        operationFinished(job);
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
              QStringLiteral("Operation is not supported by this encryption session"));
    return job;
}

EncryptionJob *EncryptedSession::encrypt(const QDomElement &) { return unsupportedJob(this); }
EncryptionJob *EncryptedSession::decrypt(const QDomElement &) { return unsupportedJob(this); }
EncryptionJob *EncryptedSession::encrypt(const QByteArray &) { return unsupportedJob(this); }
EncryptionJob *EncryptedSession::decrypt(const QByteArray &) { return unsupportedJob(this); }

EncryptionMethod::EncryptionMethod(QObject *parent) : QObject(parent) { }
EncryptionMethod::~EncryptionMethod() = default;

class EncryptionManager::Private {
public:
    QList<QPointer<EncryptionMethod>> methods;
    QSet<EncryptedSession *>          sessions;
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
            for (auto *session : std::as_const(d->sessions)) {
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
        for (auto *session : std::as_const(d->sessions)) {
            if (session && session->methodId() == id)
                session->close();
        }
        emit methodUnregistered(id);
    }
}

EncryptionMethod *EncryptionManager::method(const QString &id) const
{
    for (const auto &entry : d->methods) {
        if (entry && entry->id() == id)
            return entry;
    }
    return nullptr;
}

EncryptionMethod *EncryptionManager::methodForStanza(const QDomElement &stanza) const
{
    for (const auto &entry : d->methods) {
        if (entry && entry->capabilities().testFlag(EncryptionMethod::XmppStanza) && entry->canDecrypt(stanza))
            return entry;
    }
    return nullptr;
}

EncryptionManager::MethodsMap EncryptionManager::methods(EncryptionMethod::Capabilities caps) const
{
    MethodsMap ret;
    for (const auto &entry : d->methods) {
        if (!entry)
            continue;
        if (caps == EncryptionMethod::Capabilities() || (caps & entry->capabilities()))
            ret[entry->id()] = entry->name();
    }
    return ret;
}

Features EncryptionManager::features() const
{
    Features result;
    for (const auto &entry : d->methods) {
        if (entry)
            result += entry->features();
    }
    return result;
}

EncryptionJob *EncryptionManager::unsupported(const QString &message) const
{
    auto job = new EncryptionJob(const_cast<EncryptionManager *>(this));
    job->fail(EncryptionJob::Error::Unsupported, message);
    return job;
}

EncryptedSession *EncryptionManager::startSession(const QString &methodId, EncryptionMethod::Capability capability,
                                                  const EncryptionContext &context)
{
    auto *method = this->method(methodId);
    if (!method || !method->capabilities().testFlag(capability))
        return nullptr;

    auto *session = method->startSession(EncryptionMethod::Capabilities(capability), context);
    if (!session)
        return nullptr;
    if (session->methodId() != methodId) {
        delete session;
        return nullptr;
    }
    session->setParent(this);
    d->sessions.insert(session);
    connect(session, &QObject::destroyed, this, [this, session]() { d->sessions.remove(session); });
    return session;
}

EncryptionJob *EncryptionManager::run(EncryptedSession                                         *session,
                                      const std::function<EncryptionJob *(EncryptedSession *)> &operation)
{
    if (!session || !d->sessions.contains(session) || session->isClosing())
        return unsupported(QStringLiteral("Encryption session is unavailable"));

    EncryptionJob *job = operation(session);
    if (!job) {
        return unsupported(QStringLiteral("Encryption session returned no job"));
    }

    // Jobs have their own lifetime. A closing long-lived session remains alive
    // until every operation which uses its stored context has completed.
    job->setParent(this);
    session->track(job);
    return job;
}

EncryptionJob *EncryptionManager::runTransient(EncryptionMethod *method, EncryptionMethod::Capability capability,
                                               const EncryptionContext                                  &context,
                                               const std::function<EncryptionJob *(EncryptedSession *)> &operation)
{
    if (!method || !method->capabilities().testFlag(capability))
        return unsupported(QStringLiteral("Encryption method does not support the requested capability"));

    auto *session = startSession(method->id(), capability, context);
    if (!session)
        return unsupported(QStringLiteral("Encryption method could not create a session"));
    auto *job = run(session, operation);
    session->close();
    return job;
}

EncryptionJob *EncryptionManager::encrypt(const QString &methodId, const QDomElement &stanza,
                                          const EncryptionContext &context)
{
    auto m = method(methodId);
    if (!m)
        return unsupported(QStringLiteral("Unknown encryption method: %1").arg(methodId));
    return runTransient(m, EncryptionMethod::XmppStanza, context,
                        [&stanza](EncryptedSession *session) { return session->encrypt(stanza); });
}

EncryptionJob *EncryptionManager::decrypt(const QDomElement &stanza, const EncryptionContext &context)
{
    auto m = methodForStanza(stanza);
    if (!m)
        return unsupported(QStringLiteral("No encryption method recognizes this stanza"));
    return runTransient(m, EncryptionMethod::XmppStanza, context,
                        [&stanza](EncryptedSession *session) { return session->decrypt(stanza); });
}

EncryptionJob *EncryptionManager::encrypt(const QString &methodId, const QByteArray &data,
                                          const EncryptionContext &context)
{
    auto m = method(methodId);
    if (!m)
        return unsupported(QStringLiteral("Unknown encryption method: %1").arg(methodId));
    return runTransient(m, EncryptionMethod::DataMessage, context,
                        [&data](EncryptedSession *session) { return session->encrypt(data); });
}

EncryptionJob *EncryptionManager::decrypt(const QString &methodId, const QByteArray &data,
                                          const EncryptionContext &context)
{
    auto m = method(methodId);
    if (!m)
        return unsupported(QStringLiteral("Unknown encryption method: %1").arg(methodId));
    return runTransient(m, EncryptionMethod::DataMessage, context,
                        [&data](EncryptedSession *session) { return session->decrypt(data); });
}

EncryptionJob *EncryptionManager::encrypt(EncryptedSession *session, const QDomElement &stanza)
{
    return run(session, [&stanza](EncryptedSession *current) { return current->encrypt(stanza); });
}

EncryptionJob *EncryptionManager::decrypt(EncryptedSession *session, const QDomElement &stanza)
{
    return run(session, [&stanza](EncryptedSession *current) { return current->decrypt(stanza); });
}

EncryptionJob *EncryptionManager::encrypt(EncryptedSession *session, const QByteArray &data)
{
    return run(session, [&data](EncryptedSession *current) { return current->encrypt(data); });
}

EncryptionJob *EncryptionManager::decrypt(EncryptedSession *session, const QByteArray &data)
{
    return run(session, [&data](EncryptedSession *current) { return current->decrypt(data); });
}

} // namespace XMPP
