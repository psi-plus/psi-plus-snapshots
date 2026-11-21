/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEMEDIA_H
#define QITEMEDIA_H

#include <QObject>
#include <QPair>
#include <QVector>

// Host-owned state shared by players of the same resource. Ranges use the
// fraction of FILE BYTES, not media timestamps; they may be discontiguous.
// Availability/download updates never change the document. Setters and players
// live on the GUI thread; the host marshals notifications from worker threads.
class ITEMediaState : public QObject {
    Q_OBJECT
public:
    using Range  = QPair<qreal, qreal>; // [begin, end), normalized to [0, 1]
    using Ranges = QVector<Range>;
    // Provider-neutral media availability, independent of QMediaPlayer state
    // and of an explicit Save/Download action.
    enum class TransferStatus { Waiting, Transferring, Paused, Failed, Complete };
    explicit ITEMediaState(QObject *parent = nullptr) : QObject(parent) { }

    const Ranges &cachedRanges() const { return ranges_; }
    bool          downloading() const { return downloading_; }
    qreal         downloadProgress() const { return progress_; } // -1 means unknown
    QString       errorString() const { return error_; }
    QString       sourceError() const { return sourceError_; }
    quint64       totalBytes() const { return totalBytes_; }
    quint64       availableBytes() const { return availableBytes_; }
    TransferStatus transferStatus() const { return transferStatus_; }
    QString       transferError() const { return transferError_; }
    void setTransfer(quint64 available, quint64 total, TransferStatus status, const QString &error = {});
    void          setSourceError(const QString &error)
    {
        if (sourceError_ != error) {
            sourceError_ = error;
            emit changed();
        }
    }
    void          setCachedRanges(Ranges ranges);
    void          setDownload(bool active, qreal progress = -1, const QString &error = {});

signals:
    void changed();

private:
    Ranges  ranges_;
    bool    downloading_ = false;
    qreal   progress_    = -1;
    QString error_;
    QString sourceError_;
    quint64 totalBytes_ = 0, availableBytes_ = 0;
    TransferStatus transferStatus_ = TransferStatus::Waiting;
    QString transferError_;
};
#endif
