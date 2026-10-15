/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEVIDEOFRAME_P_H
#define QITEVIDEOFRAME_P_H

#include <QVideoFrame>
#include <QWidget>
#include <memory>

class QMediaPlayer;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QVideoSink;
#else
#include <QAbstractVideoSurface>
class ITEVideoSurface : public QAbstractVideoSurface {
    Q_OBJECT
public:
    using QAbstractVideoSurface::QAbstractVideoSurface;
    QList<QVideoFrame::PixelFormat> supportedPixelFormats(QAbstractVideoBuffer::HandleType type) const override;
    bool                            present(const QVideoFrame &frame) override;
signals:
    void frameReady(const QVideoFrame &frame, const QSize &displaySize, bool bottomUp, const QRect &sourceRect);
};
#endif

// An alien QWidget, deliberately not a QVideoWidget/window container. Native
// children promote QTextEdit's ancestors and can synchronously expose/repaint
// them from a Wayland resize. The mailbox keeps at most one undelivered frame.
class ITEVideoFrameWidget : public QWidget {
    Q_OBJECT
public:
    explicit ITEVideoFrameWidget(QWidget *parent = nullptr);
    ~ITEVideoFrameWidget() override;
    void attach(QMediaPlayer *player);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVideoSink *videoSink() const { return sink; }
#else
    ITEVideoSurface *videoSurface() const { return surface; }
#endif

protected:
    void paintEvent(QPaintEvent *) override;

private:
    struct Mailbox;
    std::shared_ptr<Mailbox> mailbox;
    QVideoFrame              frame;
    QSize                    displaySize;
    QRect                    sourceRect;
    bool                     bottomUp = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVideoSink *sink;
#else
    ITEVideoSurface *surface;
#endif
};
#endif
