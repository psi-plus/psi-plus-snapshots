#ifndef ZLIBCOMPRESSOR_H
#define ZLIBCOMPRESSOR_H

#include "zlib.h"

#include <QObject>

class QIODevice;

class ZLibCompressor : public QObject {
    Q_OBJECT

public:
    ZLibCompressor(QIODevice *device, int compression = Z_DEFAULT_COMPRESSION);
    ~ZLibCompressor();

    int write(const QByteArray &);

protected slots:
    void flush();

protected:
    int write(const QByteArray &, bool flush);

private:
    QIODevice *device_;
    z_stream * zlib_stream_;
    bool       flushed_;
};

#endif // ZLIBCOMPRESSOR_H
