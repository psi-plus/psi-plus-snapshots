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
    explicit ITEMediaState(QObject *parent = nullptr) : QObject(parent) { }

    const Ranges &cachedRanges() const { return ranges_; }
    bool          downloading() const { return downloading_; }
    qreal         downloadProgress() const { return progress_; } // -1 means unknown
    QString       errorString() const { return error_; }
    void          setCachedRanges(Ranges ranges);
    void          setDownload(bool active, qreal progress = -1, const QString &error = {});

signals:
    void changed();

private:
    Ranges  ranges_;
    bool    downloading_ = false;
    qreal   progress_    = -1;
    QString error_;
};
#endif
