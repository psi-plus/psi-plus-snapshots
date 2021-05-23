#include "blake2qt.h"

#include "blake2.h"

#include <QIODevice>

namespace XMPP {
/* Padded structs result in a compile-time error */
static_assert(sizeof(blake2s_param) == BLAKE2S_OUTBYTES, "sizeof(blake2s_param) != BLAKE2S_OUTBYTES");
static_assert(sizeof(blake2b_param) == BLAKE2B_OUTBYTES, "sizeof(blake2b_param) != BLAKE2B_OUTBYTES");

class Blake2Hash::Private {
public:
    blake2b_state state;
};

Blake2Hash::Blake2Hash(DigestSize digestSize) : d(new Private)
{
    size_t digestSizeBytes = digestSize == Digest256 ? 32 : 64;
    int    retCode         = blake2b_init(&d->state, digestSizeBytes);
    if (retCode != 0)
        d.reset();
}

Blake2Hash::Blake2Hash(Blake2Hash &&other) : d(other.d.release()) { }

Blake2Hash::~Blake2Hash() { }

bool Blake2Hash::addData(const QByteArray &data)
{
    return blake2b_update(&d->state, data.data(), size_t(data.size())) == 0;
}

bool Blake2Hash::addData(QIODevice *dev)
{
    bool isOpen = dev->isOpen();
    if (!isOpen) {
        dev->open(QIODevice::ReadOnly);
    }
    if (!dev->isOpen()) {
        return false;
    }

    bool       ret = true;
    QByteArray buf;
    // reading by 1Mb should work well with disk caches
    while ((buf = dev->read(1024 * 1024)).size() > 0)
        if (!addData(buf)) {
            ret = false;
            break;
        }

    if (!isOpen)
        dev->close();
    return ret;
}

QByteArray Blake2Hash::final()
{
    QByteArray ret;
    ret.resize(d->state.outlen);
    if (blake2b_final(&d->state, ret.data(), size_t(ret.size())) == 0)
        return ret;
    return QByteArray();
}

QByteArray Blake2Hash::compute(const QByteArray &ba, DigestSize digestSize)
{
    // otherwise try to libb2 or bundled reference implementation depending on which is available

    size_t     digestSizeBytes = digestSize == Digest256 ? 32 : 64;
    QByteArray ret(int(digestSizeBytes), Qt::Uninitialized);

    if (blake2b(ret.data(), digestSizeBytes, ba.data(), size_t(ba.size()), nullptr, 0) != 0) {
        ret.clear();
    }

    return ret;
}

QByteArray Blake2Hash::compute(QIODevice *dev, DigestSize digestSize)
{
    Blake2Hash hash(digestSize);
    if (!(hash.isValid() && hash.addData(dev)))
        return QByteArray();

    return hash.final();
}

} // namespace XMPP
