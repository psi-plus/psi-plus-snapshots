/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEVIDEO_H
#define QITEVIDEO_H

#include "qite.h"
#include <memory>

// The document contains a fixed-size placeholder only. Playback, frames and
// controls belong to opaque viewport children, never to the QTextCharFormat.
class ITEVideoController : public InteractiveTextElementController {
    Q_OBJECT
public:
    explicit ITEVideoController(InteractiveText *itc, QObject *parent = nullptr);
    ~ITEVideoController() override;

    QTextCharFormat makeFormat(const QUrl &source, ITEMediaOpener *opener = nullptr,
                               const QSize &size = QSize(320, 220)) const;
    void            insert(const QUrl &source, ITEMediaOpener *opener = nullptr, const QSize &size = QSize(320, 220));
    QSizeF          intrinsicSize(QTextDocument *, int, const QTextFormat &format) override;
    void            drawITE(QPainter *painter, const QRectF &rect, int position, const QTextFormat &format) override;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    class Private;
    std::unique_ptr<Private> d;
};
#endif
