/* SPDX-License-Identifier: Apache-2.0 */
#include "qitevideoframe_p.h"

#include <QMediaPlayer>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QVideoSink>
#else
#include <QVideoSurfaceFormat>

QList<QVideoFrame::PixelFormat> ITEVideoSurface::supportedPixelFormats(QAbstractVideoBuffer::HandleType type) const
{
    if (type != QAbstractVideoBuffer::NoHandle)
        return {};
    return { QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32, QVideoFrame::Format_ARGB32_Premultiplied,
             QVideoFrame::Format_RGB24, QVideoFrame::Format_RGB565, QVideoFrame::Format_RGB555 };
}

bool ITEVideoSurface::present(const QVideoFrame &frame)
{
    emit frameReady(frame, surfaceFormat().sizeHint(),
                    surfaceFormat().scanLineDirection() == QVideoSurfaceFormat::BottomToTop,
                    surfaceFormat().viewport());
    return true;
}
#endif

struct ITEVideoFrameWidget::Mailbox {
    QMutex               mutex;
    ITEVideoFrameWidget *target = nullptr;
    QVideoFrame          frame;
    QSize                displaySize;
    QRect                sourceRect;
    bool                 bottomUp = false;
    bool                 queued   = false;
};

ITEVideoFrameWidget::ITEVideoFrameWidget(QWidget *parent) : QWidget(parent), mailbox(new Mailbox)
{
    setObjectName(QStringLiteral("qite-video-frame"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    mailbox->target = this;
    // Producer threads touch only the mailbox. Enqueue under its lock, so the
    // destructor cannot invalidate the receiver between checking and posting.
    auto enqueue
        = [box = mailbox](const QVideoFrame &frame, const QSize &size, bool bottomUp, const QRect &sourceRect) {
              QMutexLocker lock(&box->mutex);
              auto         target = box->target;
              if (!target)
                  return;
              box->frame       = frame;
              box->displaySize = size;
              box->sourceRect  = sourceRect;
              box->bottomUp    = bottomUp;
              if (box->queued)
                  return;
              box->queued = true;
              QMetaObject::invokeMethod(
                  target,
                  [box, target] {
                      {
                          QMutexLocker lock(&box->mutex);
                          if (box->target != target)
                              return;
                          target->frame       = std::move(box->frame);
                          target->displaySize = box->displaySize;
                          target->sourceRect  = box->sourceRect;
                          target->bottomUp    = box->bottomUp;
                          box->queued         = false;
                      }
                      target->update(); // Only this opaque widget, never the text viewport.
                  },
                  Qt::QueuedConnection);
          };
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    sink = new QVideoSink(this);
    connect(
        sink, &QVideoSink::videoFrameChanged, sink,
        [enqueue](const QVideoFrame &frame) { enqueue(frame, {}, false, {}); }, Qt::DirectConnection);
#else
    surface = new ITEVideoSurface(this);
    connect(surface, &ITEVideoSurface::frameReady, surface, enqueue, Qt::DirectConnection);
#endif
}

ITEVideoFrameWidget::~ITEVideoFrameWidget()
{
    QMutexLocker lock(&mailbox->mutex);
    mailbox->target = nullptr;
    mailbox->frame  = {};
}

void ITEVideoFrameWidget::attach(QMediaPlayer *player)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    player->setVideoOutput(sink);
#else
    player->setVideoOutput(surface);
#endif
}

void ITEVideoFrameWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), Qt::black);
    if (!frame.isValid())
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVideoFrame::PaintOptions options;
    options.backgroundColor = Qt::black;
    frame.paint(&painter, rect(), options);
#else
    if (!frame.map(QAbstractVideoBuffer::ReadOnly))
        return;
    const QImage image(frame.bits(), frame.width(), frame.height(), frame.bytesPerLine(),
                       QVideoFrame::imageFormatFromPixelFormat(frame.pixelFormat()));
    QSizeF       size = displaySize.isValid() ? QSizeF(displaySize) : QSizeF(image.size());
    size.scale(this->size(), Qt::KeepAspectRatio);
    const QRectF target((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
    if (bottomUp) {
        painter.translate(0, height());
        painter.scale(1, -1);
    }
    painter.drawImage(target, image, sourceRect.isValid() ? sourceRect : image.rect());
    frame.unmap();
#endif
}
