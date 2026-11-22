/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEMEDIA_P_H
#define QITEMEDIA_P_H

#include "qitemedia.h"
#include <QPointer>
#include <QSlider>

class QPainter;
// Paint availability beneath a narrower playback stripe, keeping both visible
// even when the two fractions coincide. No byte-to-time mapping is implied.
void    itePaintMediaProgress(QPainter *, const QRectF &, const ITEMediaState *, qreal played, const QColor &,
                              bool upsideDown = false);
QString iteMediaDownloadText(const ITEMediaState *state);
// Compact, host-neutral byte-progress indicator shared by audio and video.
QWidget *iteCreateMediaTransferIndicator(ITEMediaState *state, QWidget *parent);

class ITEMediaSlider : public QSlider {
public:
    explicit ITEMediaSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) { }
    void setMediaState(ITEMediaState *state);
    void setMediaDuration(qint64 milliseconds) { durationMs_ = qMax<qint64>(0, milliseconds); }

protected:
    bool event(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    bool grooveSeekPressed_ = false;
    qint64 durationMs_ = 0;
    QPointer<ITEMediaState> state_;
    QMetaObject::Connection connection_, destroyed_;
};
#endif
