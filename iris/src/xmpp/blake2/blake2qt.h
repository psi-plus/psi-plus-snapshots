#ifndef BLAKE2QT_H
#define BLAKE2QT_H

#include <QByteArray>

namespace XMPP {

enum Blake2DigestSize {
    Blake2Digest256,
    Blake2Digest512
};

QByteArray computeBlake2Hash(const QByteArray &ba, Blake2DigestSize digestSize);

}
#endif
