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

class ITEMediaSlider : public QSlider {
public:
    explicit ITEMediaSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) { }
    void setMediaState(ITEMediaState *state);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;

private:
    QPointer<ITEMediaState> state_;
    QMetaObject::Connection connection_, destroyed_;
};
#endif
