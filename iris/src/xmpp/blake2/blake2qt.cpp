#include "blake2qt.h"
#include "blake2.h"

#include <QIODevice>

namespace XMPP {

QByteArray computeBlake2Hash(const QByteArray &ba, Blake2DigestSize digestSize)
{
    QByteArray ret;
    int retCode;
    if (digestSize == Blake2Digest256) {
        ret.reserve(BLAKE2S_OUTBYTES);
        retCode = blake2s(ret.data(), BLAKE2S_OUTBYTES, ba.data(), ba.size(), nullptr, 0);
    } else {
        ret.reserve(BLAKE2B_OUTBYTES);
        retCode = blake2b(ret.data(), BLAKE2B_OUTBYTES, ba.data(), ba.size(), nullptr, 0);
    }

    if (retCode != 0) {
        ret.clear();
    }

    return ret;
}

static QByteArray computeBlake2Hash256(QIODevice *dev)
{
    int retCode;
    blake2s_state state;

    if (!dev->isOpen()) {
        dev->open(QIODevice::ReadOnly);
    }
    if (!dev->isOpen()) {
        return QByteArray();
    }

    retCode = blake2s_init(&state, BLAKE2S_OUTBYTES);
    if (retCode != 0) {
        return QByteArray();
    }
    QByteArray buf;
    while ((buf = dev->read(64 * 1024 * 1024)).size() > 0) {
        retCode = blake2s_update(&state, buf.data(), buf.size());
        if (retCode != 0) {
            return QByteArray();
        }
    }

    QByteArray ret;
    ret.reserve(BLAKE2S_OUTBYTES);
    retCode = blake2s_final(&state, ret.data(), ret.size());
    if (retCode != 0) {
        return QByteArray();
    }

    return ret;
}

static QByteArray computeBlake2Hash512(QIODevice *dev)
{
    int retCode;
    blake2b_state state;

    if (!dev->isOpen()) {
        dev->open(QIODevice::ReadOnly);
    }
    if (!dev->isOpen()) {
        return QByteArray();
    }

    retCode = blake2b_init(&state, BLAKE2B_OUTBYTES);
    if (retCode != 0) {
        return QByteArray();
    }
    QByteArray buf;
    while ((buf = dev->read(64 * 1024 * 1024)).size() > 0) {
        retCode = blake2b_update(&state, buf.data(), buf.size());
        if (retCode != 0) {
            return QByteArray();
        }
    }

    QByteArray ret;
    ret.reserve(BLAKE2B_OUTBYTES);
    retCode = blake2b_final(&state, ret.data(), ret.size());
    if (retCode != 0) {
        return QByteArray();
    }

    return ret;
}

QByteArray computeBlake2Hash(QIODevice *dev, Blake2DigestSize digestSize)
{
    QByteArray ret;
    if (digestSize == Blake2Digest256) {
        return computeBlake2Hash256(dev);
    } else {
        return computeBlake2Hash512(dev);
    }
}

} //namespace XMPP
