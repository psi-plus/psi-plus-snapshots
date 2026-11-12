/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEVIDEO_H
#define QITEVIDEO_H

#include "qite.h"
#include <memory>
#include <QIcon>
#include <QPalette>

// The document contains a fixed-size placeholder only. Playback, frames and
// controls belong to opaque viewport children, never to the QTextCharFormat.
class ITEVideoController : public InteractiveTextElementController {
    Q_OBJECT
public:
    explicit ITEVideoController(InteractiveText *itc, QObject *parent = nullptr);
    ~ITEVideoController() override;
    // Reuse the same decoder, frame widget and controls outside a text document.
    static QWidget *createPlayer(const QUrl &, ITEMediaOpener *, QWidget *parent, std::function<void()> fullscreen = {},
                                 qint64 position = 0, bool audioPresentation = false);
    static QIcon fullscreenIcon(const QPalette &palette, bool inward);
    using WindowHandler = std::function<bool(const QUrl &, qint64, bool)>;
    void setWindowHandler(WindowHandler handler) { windowHandler_ = std::move(handler); }
    bool requestWindow(const QUrl &source, qint64 position, bool fullscreen)
    {
        const auto handler = windowHandler_;
        return handler && handler(source, position, fullscreen);
    }
    // Optional zero-interruption transfer. The host borrows the existing
    // player QWidget (and therefore its QMediaPlayer/stream), rather than
    // opening a duplicate decoder at the requested position. When finished,
    // it calls restore(), which returns false if the chat anchor is gone.
    using PlayerTransferHandler = std::function<bool(const QUrl &, QWidget *, std::function<bool()>, bool)>;
    void setPlayerTransferHandler(PlayerTransferHandler handler) { playerTransferHandler_ = std::move(handler); }
    bool requestPlayerTransfer(const QUrl &source, QWidget *player, std::function<bool()> restore, bool fullscreen)
    {
        const auto handler = playerTransferHandler_;
        return handler && handler(source, player, std::move(restore), fullscreen);
    }

    QTextCharFormat makeFormat(const QUrl &source, ITEMediaOpener *opener = nullptr,
                               const QSize &size = QSize(320, 220), bool downloadable = false) const;
    void            insert(const QUrl &source, ITEMediaOpener *opener = nullptr, const QSize &size = QSize(320, 220),
                           bool downloadable = false);
    QSizeF          intrinsicSize(QTextDocument *, int, const QTextFormat &format) override;
    void            drawITE(QPainter *painter, const QRectF &rect, int position, const QTextFormat &format) override;

signals:
    void downloadRequested(const QUrl &source);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    class Private;
    std::unique_ptr<Private> d;
    WindowHandler            windowHandler_;
    PlayerTransferHandler    playerTransferHandler_;
};
#endif
