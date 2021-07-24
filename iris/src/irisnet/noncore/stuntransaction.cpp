/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
 * Copyright (C) 2013-2021 Psi IM team
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

#include "stuntransaction.h"

#include "stunbinding.h"
#include "stunmessage.h"
#include "stuntypes.h"
#include "stunutil.h"
#include "transportaddress.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMetaType>
#include <QTime>
#include <QTimer>
#include <QtCrypto>

Q_DECLARE_METATYPE(XMPP::StunTransaction::Error)

namespace XMPP {
// parse a stun message, optionally performing validity checks.  the
//   StunMessage class itself provides parsing with validity or parsing
//   without validity, but it does not provide a way to do both together,
//   so we attempt to do that here.
// TODO: consider moving this code into StunMessage
static StunMessage parse_stun_message(const QByteArray &packet, int *validationFlags, const QByteArray &key)
{
    // ideally we shouldn't fully parse the packet more than once.  the
    //   integrity checks performed by fromBinary do not require fully
    //   parsing the packet, so we should be able to avoid most redundant
    //   processing.  fromBinary checks the fingerprint first, and we
    //   can use that knowledge to avoid duplicating integrity checks.
    int                        flags = 0;
    StunMessage::ConvertResult result;
    StunMessage                msg
        = StunMessage::fromBinary(packet, &result, StunMessage::MessageIntegrity | StunMessage::Fingerprint, key);
    if (result == StunMessage::ErrorFingerprint) {
        // if fingerprint fails, then it is the only thing that was
        //   performed and we can skip it now.
        msg = StunMessage::fromBinary(packet, &result, StunMessage::MessageIntegrity, key);
        if (result == StunMessage::ErrorMessageIntegrity) {
            // if message-integrity fails, then it is the only
            //   thing that was performed and we can skip it now
            msg = StunMessage::fromBinary(packet, &result);
            if (result == StunMessage::ConvertGood)
                flags = 0;
            else
                return msg; // null
        } else if (result == StunMessage::ConvertGood)
            flags = StunMessage::MessageIntegrity;
        else
            return msg; // null
    } else if (result == StunMessage::ErrorMessageIntegrity) {
        // fingerprint succeeded, but message-integrity failed.  parse
        //   without validation now (to skip redundant
        //   fingerprint/message-integrity checks), and assume correct
        //   fingerprint
        msg = StunMessage::fromBinary(packet, &result);
        if (result == StunMessage::ConvertGood)
            flags = StunMessage::Fingerprint;
        else
            return msg; // null
    } else if (result == StunMessage::ConvertGood)
        flags = StunMessage::MessageIntegrity | StunMessage::Fingerprint;
    else
        return msg; // null

    *validationFlags = flags;
    return msg;
}

class StunTransactionPoolPrivate : public QObject {
    Q_OBJECT

public:
    StunTransactionPool *                q;
    StunTransaction::Mode                mode;
    QSet<StunTransaction *>              transactions;
    QHash<StunTransaction *, QByteArray> transToId;
    QHash<QByteArray, StunTransaction *> idToTrans;
    bool                                 useLongTermAuth  = false;
    bool                                 needLongTermAuth = false;
    QSet<TransportAddress>               triedLongTermAuth;
    QString                              user;
    QCA::SecureArray                     pass;
    QString                              realm;
    QString                              nonce;
    int                                  debugLevel = StunTransactionPool::DL_None;

    StunTransactionPoolPrivate(StunTransactionPool *_q) : QObject(_q), q(_q) { }

    QByteArray generateId() const;
    void       insert(StunTransaction *trans);
    void       remove(StunTransaction *trans);
    void       transmit(StunTransaction *trans);
};

//----------------------------------------------------------------------------
// StunTransaction
//----------------------------------------------------------------------------
class StunTransactionPrivate : public QObject {
    Q_OBJECT

public:
    StunTransaction *q;

    StunTransactionPool::Ptr pool;
    bool                     active     = false;
    bool                     cancelling = false;
    StunTransaction::Mode    mode;
    StunMessage              origMessage;
    QByteArray               id;
    QByteArray               packet;
    TransportAddress         to_addr;

    // defaults from RFC 5389
    int     rto = 500, rc = 7, rm = 16, ti = 39500;
    int     tries;
    int     last_interval;
    QTimer *t;

    QString       stuser;
    QString       stpass;
    bool          fpRequired = false;
    QByteArray    key;
    QElapsedTimer time;

    StunTransactionPrivate(StunTransaction *_q) : QObject(_q), q(_q)
    {
        qRegisterMetaType<StunTransaction::Error>();

        t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &StunTransactionPrivate::t_timeout);
        t->setSingleShot(true);
    }

    ~StunTransactionPrivate()
    {
        if (pool)
            pool->d->remove(q);

        t->disconnect(this);
        t->setParent(nullptr);
        t->deleteLater();
    }

    void start(StunTransactionPool::Ptr _pool, const TransportAddress &toAddress)
    {
        pool    = _pool;
        mode    = pool->d->mode;
        to_addr = toAddress;

        tryRequest();
    }

    void setMessage(const StunMessage &request) { origMessage = request; }

    void retry()
    {
        Q_ASSERT(!active);
        pool->d->remove(q);

        tryRequest();
    }

    void tryRequest()
    {
        emit q->createMessage(pool->d->generateId());

        if (origMessage.isNull()) {
            // since a transaction is not cancelable nor reusable,
            //   there's no DOR-SR issue here
            QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection,
                                      Q_ARG(XMPP::StunTransaction::Error, StunTransaction::ErrorGeneric));
            return;
        }

        StunMessage out = origMessage;

        out.setClass(StunMessage::Request);
        id = QByteArray((const char *)out.id(), 12);

        if (!stuser.isEmpty()) {
            QList<StunMessage::Attribute> list = out.attributes();
            StunMessage::Attribute        attr;
            attr.type = StunTypes::USERNAME;
            attr.value
                = StunTypes::createUsername(QString::fromUtf8(StunUtil::saslPrep(stuser.toUtf8()).toByteArray()));
            list += attr;
            out.setAttributes(list);

            key = StunUtil::saslPrep(stpass.toUtf8()).toByteArray();
        } else if (!pool->d->nonce.isEmpty()) {
            QList<StunMessage::Attribute> list = out.attributes();
            {
                StunMessage::Attribute attr;
                attr.type  = StunTypes::USERNAME;
                attr.value = StunTypes::createUsername(
                    QString::fromUtf8(StunUtil::saslPrep(pool->d->user.toUtf8()).toByteArray()));
                list += attr;
            }
            {
                StunMessage::Attribute attr;
                attr.type  = StunTypes::REALM;
                attr.value = StunTypes::createRealm(pool->d->realm);
                list += attr;
            }
            {
                StunMessage::Attribute attr;
                attr.type  = StunTypes::NONCE;
                attr.value = StunTypes::createNonce(pool->d->nonce);
                list += attr;
            }
            out.setAttributes(list);

            QCA::SecureArray buf;
            buf += StunUtil::saslPrep(pool->d->user.toUtf8());
            buf += QByteArray(1, ':');
            buf += StunUtil::saslPrep(pool->d->realm.toUtf8());
            buf += QByteArray(1, ':');
            buf += StunUtil::saslPrep(pool->d->pass);

            key = QCA::Hash("md5").process(buf).toByteArray();
        }

        if (!key.isEmpty())
            packet = out.toBinary(StunMessage::MessageIntegrity | StunMessage::Fingerprint, key);
        else
            packet = out.toBinary(StunMessage::Fingerprint);

        if (packet.isEmpty()) {
            // since a transaction is not cancelable nor reusable,
            //   there's no DOR-SR issue here
            QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection,
                                      Q_ARG(XMPP::StunTransaction::Error, StunTransaction::ErrorGeneric));
            return;
        }

        active = true;
        tries  = 1; // we transmit immediately here, so count it

        if (mode == StunTransaction::Udp) {
            last_interval = rm * rto;
            t->start(rto);
            rto *= 2;
        } else if (mode == StunTransaction::Tcp) {
            t->start(ti);
        } else
            Q_ASSERT(0);

        time.start();
        pool->d->insert(q);
        transmit();
    }

private slots:
    void t_timeout()
    {
        if (cancelling) {
            q->deleteLater();
            return;
        }
        if (mode == StunTransaction::Tcp || tries == rc) {
            pool->d->remove(q);
            emit q->error(StunTransaction::ErrorTimeout);
            return;
        }

        ++tries;
        if (tries == rc) {
            t->start(last_interval);
        } else {
            t->start(rto);
            rto *= 2;
        }

        QString dbg;
        if (to_addr.isValid())
            dbg += QString("to=(%1)").arg(to_addr);

        emit pool->debugLine(QString("stun transaction %1 timeout. retransmitting..").arg(dbg));
        transmit();
    }

private:
    void transmit()
    {
        if (pool->d->debugLevel >= StunTransactionPool::DL_Packet) {
            QString str = QString("STUN SEND: elapsed=") + QString::number(time.elapsed());
            if (to_addr.isValid())
                str += QString(" to=(%1)").arg(to_addr);
            emit pool->debugLine(str);

            StunMessage msg = StunMessage::fromBinary(packet);
            emit        pool->debugLine(StunTypes::print_packet_str(msg));
        }

        pool->d->transmit(q);
    }

    bool checkActiveAndFrom(const TransportAddress &from_addr)
    {
        if (!active)
            return false;

        return !to_addr.isValid() || (to_addr == from_addr);
    }

    void processIncoming(const StunMessage &msg, bool authed, const TransportAddress &from_addr)
    {
        active = false;
        t->stop();
        if (cancelling) {
            q->deleteLater();
            return;
        }

        if (pool->d->debugLevel >= StunTransactionPool::DL_Packet)
            emit pool->debugLine(QString("matched incoming response to existing request.  elapsed=")
                                 + QString::number(time.elapsed()));

        // will be set to true when receiving an Unauthorized error
        bool unauthError = false;

        bool triedLongTermAuth = pool->d->triedLongTermAuth.contains(from_addr);
        if (msg.mclass() == StunMessage::ErrorResponse && pool->d->useLongTermAuth) {
            // we'll handle certain error codes at this layer
            int     code;
            QString reason;
            if (StunTypes::parseErrorCode(msg.attribute(StunTypes::ERROR_CODE), &code, &reason)) {
                if (code == StunTypes::Unauthorized)
                    unauthError = true;

                if (unauthError && !triedLongTermAuth) {
                    QString realm;
                    QString nonce;
                    if (StunTypes::parseRealm(msg.attribute(StunTypes::REALM), &realm)
                        && StunTypes::parseRealm(msg.attribute(StunTypes::NONCE), &nonce)) {
                        // always set these to the latest received values,
                        //   which will be used for all transactions
                        //   once creds are provided.
                        if (pool->d->realm.isEmpty())
                            pool->d->realm = realm;
                        pool->d->nonce = nonce;

                        if (!pool->d->needLongTermAuth) {
                            if (!pool->d->user.isEmpty()) {
                                // creds already set?  use them
                                pool->d->triedLongTermAuth.insert(from_addr);
                                retry();
                            } else {
                                // else ask the user
                                pool->d->triedLongTermAuth.insert(from_addr);
                                emit pool->needAuthParams(from_addr);
                            }
                        }
                        return;
                    }
                } else if (code == StunTypes::StaleNonce && triedLongTermAuth) {
                    QString nonce;
                    if (StunTypes::parseNonce(msg.attribute(StunTypes::NONCE), &nonce) && nonce != pool->d->nonce) {
                        pool->d->nonce = nonce;
                        retry();
                        return;
                    }
                }
            }
        }

        // require message integrity when auth is used
        if (!unauthError && (!stuser.isEmpty() || triedLongTermAuth) && !authed)
            return;

        pool->d->remove(q);
        emit q->finished(msg);
    }

public:
    bool writeIncomingMessage(const StunMessage &msg, const TransportAddress &from_addr)
    {
        if (!checkActiveAndFrom(from_addr))
            return false;

        // if a StunMessage is passed directly to us then we assume
        //   the user has authenticated the message as necessary
        processIncoming(msg, true, from_addr);
        return true;
    }

    bool writeIncomingMessage(const QByteArray &packet, bool *notStun, const TransportAddress &from_addr)
    {
        if (!checkActiveAndFrom(from_addr)) {
            // could be STUN, don't really know for sure
            *notStun = false;
            return false;
        }

        int         validationFlags = 0;
        StunMessage msg             = parse_stun_message(packet, &validationFlags, key);
        if (msg.isNull()) {
            // packet doesn't parse at all, surely not STUN
            *notStun = true;
            return false;
        }

        if (fpRequired && !(validationFlags & StunMessage::Fingerprint)) {
            // fingerprint failed when required.  consider the
            //   packet to be surely not STUN
            *notStun = true;
            return false;
        }

        processIncoming(msg, (validationFlags & StunMessage::MessageIntegrity) != 0, from_addr);
        return true;
    }

public slots:
    void continueAfterParams()
    {
        if (cancelling)
            return;
        retry();
    }
};

StunTransaction::StunTransaction(QObject *parent) : QObject(parent) { d = new StunTransactionPrivate(this); }

StunTransaction::~StunTransaction() { delete d; }

void StunTransaction::start(StunTransactionPool *pool, const TransportAddress &toAddress)
{
    Q_ASSERT(!d->active);
    d->start(pool->sharedFromThis(), toAddress);
}

void StunTransaction::cancel() { d->cancelling = true; }

void StunTransaction::setMessage(const StunMessage &request) { d->setMessage(request); }

void StunTransaction::setRTO(int i)
{
    Q_ASSERT(!d->active);
    d->rto = i;
}

void StunTransaction::setRc(int i)
{
    Q_ASSERT(!d->active);
    d->rc = i;
}

void StunTransaction::setRm(int i)
{
    Q_ASSERT(!d->active);
    d->rm = i;
}

void StunTransaction::setTi(int i)
{
    Q_ASSERT(!d->active);
    d->ti = i;
}

void StunTransaction::setShortTermUsername(const QString &username) { d->stuser = username; }

void StunTransaction::setShortTermPassword(const QString &password) { d->stpass = password; }

void StunTransaction::setFingerprintRequired(bool enabled) { d->fpRequired = enabled; }

//----------------------------------------------------------------------------
// StunTransactionPool
//----------------------------------------------------------------------------
QByteArray StunTransactionPoolPrivate::generateId() const
{
    QByteArray id;

    do {
        id = QCA::Random::randomArray(12).toByteArray();
    } while (idToTrans.contains(id));

    return id;
}

void StunTransactionPoolPrivate::insert(StunTransaction *trans)
{
    Q_ASSERT(!trans->d->id.isEmpty());

    transactions.insert(trans);
    QByteArray id = trans->d->id;
    transToId.insert(trans, id);
    idToTrans.insert(id, trans);
}

void StunTransactionPoolPrivate::remove(StunTransaction *trans)
{
    if (transactions.contains(trans)) {
        transactions.remove(trans);
        QByteArray id = transToId.value(trans);
        transToId.remove(trans);
        idToTrans.remove(id);
    }
}

void StunTransactionPoolPrivate::transmit(StunTransaction *trans)
{
    emit q->outgoingMessage(trans->d->packet, trans->d->to_addr);
}

StunTransactionPool::StunTransactionPool(StunTransaction::Mode mode)
{
    d       = new StunTransactionPoolPrivate(this);
    d->mode = mode;
}

StunTransactionPool::~StunTransactionPool()
{
    qDeleteAll(
        findChildren<StunBinding *>()); // early remove of binding since they require alive pool (should fix one crash)
    delete d;
}

StunTransaction::Mode StunTransactionPool::mode() const { return d->mode; }

bool StunTransactionPool::writeIncomingMessage(const StunMessage &msg, const TransportAddress &addr)
{
    if (d->debugLevel >= DL_Packet) {
        QString str = "STUN RECV";
        if (addr.isValid())
            str += QString(" from=(%1)").arg(addr);
        emit debugLine(str);
        emit debugLine(StunTypes::print_packet_str(msg));
    }

    QByteArray         id     = QByteArray::fromRawData((const char *)msg.id(), 12);
    StunMessage::Class mclass = msg.mclass();

    if (mclass != StunMessage::SuccessResponse && mclass != StunMessage::ErrorResponse)
        return false;

    StunTransaction *trans = d->idToTrans.value(id);
    if (!trans)
        return false;

    return trans->d->writeIncomingMessage(msg, addr);
}

bool StunTransactionPool::writeIncomingMessage(const QByteArray &packet, bool *notStun, const TransportAddress &addr)
{
    if (!StunMessage::isProbablyStun(packet)) {
        // basic stun check failed?  surely not STUN
        if (notStun)
            *notStun = true;
        return false;
    }

    if (d->debugLevel >= DL_Packet) {
        StunMessage msg = StunMessage::fromBinary(packet);
        QString     str = "STUN RECV";
        if (addr.isValid())
            str += QString(" from=(%1)").arg(addr);
        emit debugLine(str);
        emit debugLine(StunTypes::print_packet_str(msg));
    }

    // isProbablyStun ensures the packet is 20 bytes long, so we can safely
    //   safely extract out the transaction id from the raw packet
    QByteArray id = QByteArray((const char *)packet.data() + 8, 12);

    StunMessage::Class mclass = StunMessage::extractClass(packet);

    if (mclass != StunMessage::SuccessResponse && mclass != StunMessage::ErrorResponse) {
        // could be STUN, don't really know for sure
        if (notStun)
            *notStun = false;
        return false;
    }

    StunTransaction *trans = d->idToTrans.value(id);
    if (!trans) {
        // could be STUN, don't really know for sure
        if (notStun)
            *notStun = false;
        return false;
    }

    bool _notStun = false;
    bool ret      = trans->d->writeIncomingMessage(packet, &_notStun, addr);
    if (!ret && notStun)
        *notStun = _notStun;
    return ret;
}

void StunTransactionPool::setLongTermAuthEnabled(bool enabled) { d->useLongTermAuth = enabled; }

QString StunTransactionPool::realm() const { return d->realm; }

void StunTransactionPool::setUsername(const QString &username) { d->user = username; }

void StunTransactionPool::setPassword(const QCA::SecureArray &password) { d->pass = password; }

void StunTransactionPool::setRealm(const QString &realm) { d->realm = realm; }

void StunTransactionPool::continueAfterParams(const TransportAddress &addr)
{
    if (d->debugLevel >= DL_Info) {
        emit debugLine("continue after params:");
        emit debugLine(QString("  U=[%1]").arg(d->user));
        emit debugLine(QString("  P=[%1]").arg(d->pass.data()));
        emit debugLine(QString("  R=[%1]").arg(d->realm));
        emit debugLine(QString("  N=[%1]").arg(d->nonce));
    }

    Q_ASSERT(d->useLongTermAuth);
    Q_ASSERT(d->needLongTermAuth);
    Q_ASSERT(!d->triedLongTermAuth.contains(addr));

    d->needLongTermAuth = false;
    d->triedLongTermAuth.insert(addr);

    for (StunTransaction *trans : qAsConst(d->transactions)) {
        // the only reason an inactive transaction would be in the
        //   list is if it is waiting for an auth retry
        if (!trans->d->active && !trans->d->cancelling) {
            // use queued call to prevent all sorts of DOR-SS
            //   nastiness
            QMetaObject::invokeMethod(trans->d, "continueAfterParams", Qt::QueuedConnection);
        }
    }
}

QByteArray StunTransactionPool::generateId() const { return d->generateId(); }

void StunTransactionPool::setDebugLevel(DebugLevel level) { d->debugLevel = level; }

} // namespace XMPP

#include "stuntransaction.moc"
