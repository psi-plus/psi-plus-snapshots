#include "blake2qt.h"
#include "blake2.h"

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
        retCode = blake2s(ret.data(), BLAKE2B_OUTBYTES, ba.data(), ba.size(), nullptr, 0);
    }

    if (retCode != 0) {
        ret.clear();
    }

    return ret;
}

} //namespace XMPP
