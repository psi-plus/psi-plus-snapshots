#ifndef BLAKE2QT_H
#define BLAKE2QT_H

#include <QByteArray>

#include <memory>

class QIODevice;

namespace XMPP {

class Blake2Hash {
public:
    enum DigestSize { Digest256, Digest512 };

    Blake2Hash(DigestSize digestSize);
    Blake2Hash(Blake2Hash &&other);
    ~Blake2Hash();

    bool       addData(const QByteArray &data);
    bool       addData(QIODevice *dev);
    QByteArray final();
    bool       isValid() const { return d != nullptr; }

    static QByteArray compute(const QByteArray &ba, DigestSize digestSize);
    static QByteArray compute(QIODevice *dev, DigestSize digestSize);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

#endif // BLAKE2QT_H
