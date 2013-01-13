#ifndef ZLIBDECOMPRESSOR_H
#define ZLIBDECOMPRESSOR_H

#include <QObject>

#include "zlib.h"

class QIODevice;

class ZLibDecompressor : public QObject
{
	Q_OBJECT

public:
	ZLibDecompressor(QIODevice* device);
	~ZLibDecompressor();

	qint64 write(const QByteArray&);

protected slots:
	void flush();

protected:
	qint64 write(const QByteArray&, bool flush);

private:
	QIODevice* device_;
	z_stream* zlib_stream_;
	bool flushed_;
};

#endif
