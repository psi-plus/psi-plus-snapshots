#ifndef BLAKE2QT_H
#define BLAKE2QT_H

#include <QByteArray>

class QIODevice;

namespace XMPP {
enum Blake2DigestSize {
    Blake2Digest256,
    Blake2Digest512
};

QByteArray computeBlake2Hash(const QByteArray &ba, Blake2DigestSize digestSize);
QByteArray computeBlake2Hash(QIODevice *dev, Blake2DigestSize digestSize);
} // namespace XMPP

#endif // BLAKE2QT_H
