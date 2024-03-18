/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "stunbinding.h"

#include "stunmessage.h"
#include "stuntransaction.h"
#include "stuntypes.h"
#include "transportaddress.h"

namespace XMPP {
class StunBinding::Private : public QObject {
    Q_OBJECT

public:
    StunBinding                     *q;
    StunTransactionPool::Ptr         pool;
    std::unique_ptr<StunTransaction> trans;
    TransportAddress                 stunAddr;
    TransportAddress                 addr;
    QString                          errorString;
    bool    use_extPriority = false, use_extIceControlling = false, use_extIceControlled = false;
    quint32 extPriority       = 0;
    bool    extUseCandidate   = false;
    quint64 extIceControlling = 0, extIceControlled = 0;
    QString stuser, stpass;
    bool    fpRequired = false;

    Private(StunBinding *_q) : QObject(_q), q(_q) { }

    ~Private() { }

    void start(const TransportAddress &_addr = TransportAddress())
    {
        Q_ASSERT(!trans);

        stunAddr = _addr;

        trans.reset(new StunTransaction());
        connect(trans.get(), &StunTransaction::createMessage, this, &Private::trans_createMessage);
        connect(trans.get(), &StunTransaction::finished, this, &Private::trans_finished);
        connect(trans.get(), &StunTransaction::error, this, &Private::trans_error);

        if (!stuser.isEmpty()) {
            trans->setShortTermUsername(stuser);
            trans->setShortTermPassword(stpass);
        }

        trans->setFingerprintRequired(fpRequired);

        trans->start(pool.data(), stunAddr);
    }

    void cancel()
    {
        if (!trans)
            return;
        auto t = trans.release();
        t->disconnect(this);
        t->cancel(); // will self-delete the transaction either on incoming or timeout
        // just in case those too
        addr = TransportAddress();
        errorString.clear();

        // now the binding can be reused
    }

private slots:
    void trans_createMessage(const QByteArray &transactionId)
    {
        StunMessage message;
        message.setMethod(StunTypes::Binding);
        message.setId((const quint8 *)transactionId.data());

        QList<StunMessage::Attribute> list;

        if (use_extPriority) {
            StunMessage::Attribute a;
            a.type  = StunTypes::PRIORITY;
            a.value = StunTypes::createPriority(extPriority);
            list += a;
        }

        if (extUseCandidate) {
            StunMessage::Attribute a;
            a.type = StunTypes::USE_CANDIDATE;
            list += a;
        }

        if (use_extIceControlling) {
            StunMessage::Attribute a;
            a.type  = StunTypes::ICE_CONTROLLING;
            a.value = StunTypes::createIceControlling(extIceControlling);
            list += a;
        }

        if (use_extIceControlled) {
            StunMessage::Attribute a;
            a.type  = StunTypes::ICE_CONTROLLED;
            a.value = StunTypes::createIceControlled(extIceControlled);
            list += a;
        }

        message.setAttributes(list);

        trans->setMessage(message);
    }

    void trans_finished(const XMPP::StunMessage &response)
    {
        trans.reset();

        bool    error = false;
        int     code;
        QString reason;
        if (response.mclass() == StunMessage::ErrorResponse) {
            if (!StunTypes::parseErrorCode(response.attribute(StunTypes::ERROR_CODE), &code, &reason)) {
                errorString = "Unable to parse ERROR-CODE in error response.";
                emit q->error(StunBinding::ErrorProtocol);
                return;
            }

            error = true;
        }

        if (error) {
            errorString = reason;
            if (code == StunTypes::RoleConflict)
                emit q->error(StunBinding::ErrorConflict);
            else
                emit q->error(StunBinding::ErrorRejected);
            return;
        }

        TransportAddress saddr;

        QByteArray val;
        val = response.attribute(StunTypes::XOR_MAPPED_ADDRESS);
        if (!val.isNull()) {
            if (!StunTypes::parseXorMappedAddress(val, response.magic(), response.id(), saddr)) {
                errorString = "Unable to parse XOR-MAPPED-ADDRESS response.";
                emit q->error(StunBinding::ErrorProtocol);
                return;
            }
        } else {
            val = response.attribute(StunTypes::MAPPED_ADDRESS);
            if (!val.isNull()) {
                if (!StunTypes::parseMappedAddress(val, saddr)) {
                    errorString = "Unable to parse MAPPED-ADDRESS response.";
                    emit q->error(StunBinding::ErrorProtocol);
                    return;
                }
            } else {
                errorString = "Response does not contain XOR-MAPPED-ADDRESS or MAPPED-ADDRESS.";
                emit q->error(StunBinding::ErrorProtocol);
                return;
            }
        }

        addr = saddr;
        emit q->success();
    }

    void trans_error(XMPP::StunTransaction::Error e)
    {
        trans.reset();

        if (e == StunTransaction::ErrorTimeout) {
            errorString = "Request timed out.";
            emit q->error(StunBinding::ErrorTimeout);
        } else {
            errorString = "Generic transaction error.";
            emit q->error(StunBinding::ErrorGeneric);
        }
    }
};

StunBinding::StunBinding(StunTransactionPool *pool) : QObject(pool)
{
    d       = new Private(this);
    d->pool = pool->sharedFromThis();
}

StunBinding::~StunBinding() { delete d; }

void StunBinding::setPriority(quint32 i)
{
    d->use_extPriority = true;
    d->extPriority     = i;
}

quint32 StunBinding::priority() const { return d->extPriority; }

void StunBinding::setUseCandidate(bool enabled) { d->extUseCandidate = enabled; }

bool StunBinding::useCandidate() const { return d->extUseCandidate; }

void StunBinding::setIceControlling(quint64 i)
{
    d->use_extIceControlling = true;
    d->extIceControlling     = i;
}

void StunBinding::setIceControlled(quint64 i)
{
    d->use_extIceControlled = true;
    d->extIceControlled     = i;
}

void StunBinding::setShortTermUsername(const QString &username) { d->stuser = username; }

void StunBinding::setShortTermPassword(const QString &password) { d->stpass = password; }

void StunBinding::setFingerprintRequired(bool enabled) { d->fpRequired = enabled; }

void StunBinding::start() { d->start(); }

void StunBinding::start(const TransportAddress &addr) { d->start(addr); }

void StunBinding::cancel() { d->cancel(); }

const TransportAddress &StunBinding::reflexiveAddress() const { return d->addr; }

QString StunBinding::errorString() const { return d->errorString; }
} // namespace XMPP

#include "stunbinding.moc"
