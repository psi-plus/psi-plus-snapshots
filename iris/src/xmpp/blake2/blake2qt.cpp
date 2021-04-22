#include "blake2qt.h"

#include "blake2.h"

#include <QIODevice>

namespace XMPP {
/* Padded structs result in a compile-time error */
static_assert(sizeof(blake2s_param) == BLAKE2S_OUTBYTES, "sizeof(blake2s_param) != BLAKE2S_OUTBYTES");
static_assert(sizeof(blake2b_param) == BLAKE2B_OUTBYTES, "sizeof(blake2b_param) != BLAKE2B_OUTBYTES");

QByteArray computeBlake2Hash(const QByteArray &ba, Blake2DigestSize digestSize)
{
    // otherwise try to libb2 or bundled reference implementation depending on which is available

    size_t     digestSizeBytes = digestSize == Blake2Digest256 ? 32 : 64;
    QByteArray ret(int(digestSizeBytes), Qt::Uninitialized);

    if (blake2b(ret.data(), digestSizeBytes, ba.data(), size_t(ba.size()), nullptr, 0) != 0) {
        ret.clear();
    }

    return ret;
}

QByteArray computeBlake2Hash(QIODevice *dev, Blake2DigestSize digestSize)
{
    if (!dev->isOpen()) {
        dev->open(QIODevice::ReadOnly);
    }
    if (!dev->isOpen()) {
        return QByteArray();
    }

    // otherwise try to libb2 or bundled reference implementation depending on which is available

    size_t        digestSizeBytes = digestSize == Blake2Digest256 ? 32 : 64;
    blake2b_state state;
    int           retCode = blake2b_init(&state, digestSizeBytes);
    if (retCode != 0) {
        return QByteArray();
    }
    QByteArray buf;
    // reading by 1Mb should work well with disk caches
    while ((buf = dev->read(1024 * 1024)).size() > 0) {
        retCode = blake2b_update(&state, buf.data(), size_t(buf.size()));
        if (retCode != 0) {
            return QByteArray();
        }
    }

    QByteArray ret;
    ret.resize(int(digestSizeBytes));
    retCode = blake2b_final(&state, ret.data(), size_t(ret.size()));
    if (retCode != 0) {
        return QByteArray();
    }

    return ret;
}

} // namespace XMPP
