/* SPDX-License-Identifier: Apache-2.0 */
#include "qitevideo.h"
#include "qitemedia_p.h"
#include "qitemediaplayer_p.h"
#include "qitevideoframe_p.h"

#include <QAbstractTextDocumentLayout>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QPainter>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <functional>

Q_DECLARE_METATYPE(QPointer<ITEMediaOpener>)

namespace {
enum Property { Source = InteractiveTextFormat::UserProperty, Opener, Size, Downloadable };

class VideoControls : public QWidget {
public:
    explicit VideoControls(QWidget *parent) : QWidget(parent) { setAttribute(Qt::WA_OpaquePaintEvent); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }
};

// This top-level window owns the VideoPlayer while it is undocked. The chat
// controller holds only a weak pointer, so chat-log pruning cannot stop playback.
class VideoWindow : public QWidget {
public:
    VideoWindow(QWidget *owner, QWidget *frame, QWidget *controls, std::function<void()> close,
                std::function<void()> exitFullscreen) :
        QWidget(nullptr, Qt::Window), owner(owner), frame(frame), controls(controls),
        closeAction(std::move(close)), exitFullscreenAction(std::move(exitFullscreen))
    {
        setObjectName(QStringLiteral("qite-video-window"));
        setWindowTitle(ITEVideoController::tr("Video"));
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMouseTracking(true);
        owner->setParent(this);
        owner->show();
        const auto widgets = owner->findChildren<QWidget *>();
        for (auto widget : widgets) {
            widget->setMouseTracking(true);
            widget->installEventFilter(this);
        }
        hideTimer.setSingleShot(true);
        hideTimer.setInterval(2000);
        connect(&hideTimer, &QTimer::timeout, this, [this] {
            const auto slider = this->controls->findChild<QSlider *>();
            if (this->controls->underMouse() || (slider && slider->isSliderDown())) {
                hideTimer.start();
                return;
            }
            this->controls->hide();
            this->frame->setCursor(Qt::BlankCursor);
            setCursor(Qt::BlankCursor);
        });
        auto escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(escape, &QShortcut::activated, this, [this] {
            if (fullscreenMode)
                requestExitFullscreen();
        });
    }

    bool isFullscreenMode() const { return fullscreenMode; }

    void setFullscreenMode(bool enable)
    {
        if (fullscreenMode == enable)
            return;
        fullscreenMode = enable;
        hideTimer.stop();
        frame->unsetCursor();
        unsetCursor();
        if (enable) {
            owner->layout()->removeWidget(frame);
            owner->layout()->removeWidget(controls);
            frame->setParent(this);
            controls->setParent(this);
            frame->show();
            setObjectName(QStringLiteral("qite-video-fullscreen"));
            showFullScreen();
            resizeChildren();
            showControls();
        } else {
            frame->setParent(owner);
            controls->setParent(owner);
            auto layout = static_cast<QBoxLayout *>(owner->layout());
            layout->addWidget(frame, 1);
            layout->addWidget(controls);
            frame->show();
            controls->show();
            setObjectName(QStringLiteral("qite-video-window"));
            showNormal();
            resizeChildren();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }

    void resizeEvent(QResizeEvent *) override { resizeChildren(); }

    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();
        if (closeAction)
            closeAction();
    }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() != QEvent::WindowStateChange || !fullscreenMode)
            return;
        if (isFullScreen()) {
            wasFullscreen = true;
        } else if (wasFullscreen && !isMinimized()) {
            QTimer::singleShot(0, this, [this] {
                if (fullscreenMode && !isFullScreen() && !isMinimized())
                    requestExitFullscreen();
            });
        }
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (fullscreenMode
            && (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter
                || event->type() == QEvent::KeyPress))
            showControls();
        return false;
    }

    void mouseMoveEvent(QMouseEvent *) override
    {
        if (fullscreenMode)
            showControls();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (fullscreenMode && event->key() == Qt::Key_Escape) {
            event->accept();
            requestExitFullscreen();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    void resizeChildren()
    {
        if (fullscreenMode) {
            frame->setGeometry(rect());
            const int height = controls->sizeHint().height();
            controls->setGeometry(12, qMax(0, this->height() - height - 12), qMax(0, width() - 24), height);
            controls->raise();
        } else {
            owner->setGeometry(rect());
        }
    }

    void showControls()
    {
        if (!fullscreenMode)
            return;
        frame->unsetCursor();
        setCursor(Qt::ArrowCursor);
        controls->show();
        controls->raise();
        hideTimer.start();
    }

    void requestExitFullscreen()
    {
        if (exitFullscreenAction)
            exitFullscreenAction();
    }

    QWidget              *owner;
    QWidget              *frame;
    QWidget              *controls;
    std::function<void()> closeAction;
    std::function<void()> exitFullscreenAction;
    QTimer                hideTimer;
    bool                  fullscreenMode = false;
    bool                  wasFullscreen  = false;
};

class VideoPlayer : public QWidget {
public:
    VideoPlayer(const QTextFormat &format, QWidget *parent, ITEVideoController *controller,
                std::function<void()> docked) :
        QWidget(parent), source(format.property(Source).toUrl()), originalSource(source),
        opener(format.property(Opener).value<QPointer<ITEMediaOpener>>()), requiresOpener(format.hasProperty(Opener)),
        inlineViewport(parent), downloadController(controller), onDocked(std::move(docked))
    {
        setObjectName(QStringLiteral("qite-video-player"));
        setCursor(Qt::ArrowCursor);
        // This is essential: frame/control updates must not propagate the
        // QTextEdit background into the player and repaint the text beneath it.
        setAttribute(Qt::WA_OpaquePaintEvent);
        auto palette = this->palette();
        palette.setColor(QPalette::Window, Qt::black);
        palette.setColor(QPalette::WindowText, Qt::white);
        setPalette(palette);
        layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        video = new ITEVideoFrameWidget(this);
        video->installEventFilter(this);
        layout->addWidget(video, 1);
        controls = new VideoControls(this);
        controls->setObjectName(QStringLiteral("qite-video-controls"));
        controls->setPalette(palette);
        auto row = new QHBoxLayout(controls);
        row->setContentsMargins(6, 4, 6, 4);
        button = new QToolButton(controls);
        button->setObjectName(QStringLiteral("qite-video-play"));
        button->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        button->setToolTip(ITEVideoController::tr("Play"));
        button->setAccessibleName(button->toolTip());
        row->addWidget(button);
        slider     = new ITEMediaSlider(controls);
        mediaState = opener ? opener->mediaState(source) : nullptr;
        if (mediaState)
            connect(mediaState, &ITEMediaState::changed, this, [this] {
                if (player && mediaState && !mediaState->sourceError().isEmpty()) {
                    player->stop();
                    showError(mediaState->sourceError());
                }
            });
        slider->setMediaState(mediaState);
        slider->setRange(0, 10000);
        slider->setEnabled(false);
        slider->setAccessibleName(ITEVideoController::tr("Video position"));
        row->addWidget(slider, 1);
        time = new QLabel(QStringLiteral("0:00"), controls);
        // A QTextEdit stylesheet may override child palettes. Give the time an
        // explicit foreground on our opaque black background as well.
        time->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
        row->addWidget(time);
        if (format.boolProperty(Downloadable)) {
            downloadButton = new QToolButton(controls);
            downloadButton->setObjectName(QStringLiteral("qite-video-download"));
            downloadButton->setIcon(
                QIcon::fromTheme(QStringLiteral("document-save"), style()->standardIcon(QStyle::SP_DialogSaveButton)));
            row->addWidget(downloadButton);
            connect(downloadButton, &QToolButton::clicked, controller,
                    [controller, source = originalSource] { emit controller->downloadRequested(source); });
            auto updateDownload = [this] {
                downloadButton->setEnabled(downloadController && (!requiresOpener || opener)
                                           && (!mediaState || !mediaState->downloading()));
                downloadButton->setToolTip(iteMediaDownloadText(mediaState));
                downloadButton->setAccessibleName(downloadButton->toolTip());
            };
            if (mediaState) {
                connect(mediaState, &ITEMediaState::changed, this, updateDownload);
                connect(mediaState, &QObject::destroyed, this, updateDownload);
            }
            connect(controller, &QObject::destroyed, this, updateDownload);
            updateDownload();
        }
        windowButton = new QToolButton(controls);
        windowButton->setObjectName(QStringLiteral("qite-video-window-button"));
        windowButton->setIcon(QIcon::fromTheme(QStringLiteral("window-new"),
                                               style()->standardIcon(QStyle::SP_TitleBarNormalButton)));
        updateWindowButton();
        row->addWidget(windowButton);
        fullscreenButton = new QToolButton(controls);
        fullscreenButton->setObjectName(QStringLiteral("qite-video-fullscreen-button"));
        updateFullscreenButton();
        row->addWidget(fullscreenButton);
        layout->addWidget(controls);
        connect(windowButton, &QToolButton::clicked, this, [this] {
            if (!floating) {
                openWindow(false);
            } else if (floating->isFullscreenMode()) {
                // The pop-out button turns a fullscreen player into a regular
                // standalone window, without reattaching it to the chat.
                returnInlineOnFullscreenExit = false;
                floating->setFullscreenMode(false);
                updateFullscreenButton();
                updateWindowButton();
            } else if (inlineViewport) {
                dock();
            } else {
                floating->raise();
                floating->activateWindow();
            }
        });
        connect(fullscreenButton, &QToolButton::clicked, this, [this] {
            if (floating && floating->isFullscreenMode())
                leaveFullscreen();
            else
                enterFullscreen();
        });
        connect(button, &QToolButton::clicked, this, [this] { toggle(); });
        auto seek = [this] {
            if (player && player->isSeekable())
                player->setPosition(player->duration() * slider->value() / 10000);
        };
        connect(slider, &QSlider::sliderReleased, this, seek);
        connect(slider, &QSlider::valueChanged, this, [this, seek] {
            // Keyboard/page-step actions seek too; dragging seeks on release.
            if (!slider->isSliderDown())
                seek();
        });
        if (opener)
            connect(opener, &QObject::destroyed, this, [this] { sourceUnavailable(); });
        else if (requiresOpener)
            sourceUnavailable();
    }

    ~VideoPlayer() override
    {
        // The floating VideoWindow, if present, owns this VideoPlayer.
        // Detach the device/output before closing an opener-owned stream.
        delete player;
        if (stream && opener)
            opener->close(stream);
    }

    void pause()
    {
        if (player)
            player->pause();
    }

    bool isFloating() const { return !floating.isNull(); }

    void orphan()
    {
        // Called when the document loses its attachment or its controller is
        // destroyed. A detached window remains the player's sole owner.
        inlineViewport = nullptr;
        onDocked = {};
        returnInlineOnFullscreenExit = false;
        updateWindowButton();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }
    void hideEvent(QHideEvent *event) override
    {
        if (!floating && !movingToWindow)
            pause();
        QWidget::hideEvent(event);
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == video && event->type() == QEvent::MouseButtonDblClick) {
            fullscreenButton->click();
            return true;
        }
        return false;
    }

private:
    void updateFullscreenButton()
    {
        const bool fullscreen = floating && floating->isFullscreenMode();
        fullscreenButton->setIcon(
            style()->standardIcon(fullscreen ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
        fullscreenButton->setToolTip(fullscreen ? ITEVideoController::tr("Exit full screen")
                                                : ITEVideoController::tr("Full screen"));
        fullscreenButton->setAccessibleName(fullscreenButton->toolTip());
    }

    void updateWindowButton()
    {
        const bool fullscreen = floating && floating->isFullscreenMode();
        windowButton->setToolTip(fullscreen ? ITEVideoController::tr("Open in separate window")
                                     : floating && inlineViewport ? ITEVideoController::tr("Return to chat")
                                     : floating ? ITEVideoController::tr("Video window")
                                                : ITEVideoController::tr("Open in separate window"));
        // With no chat anchor a normal detached window is already the only
        // place the player can live; there is nowhere for this button to go.
        windowButton->setEnabled(!floating || inlineViewport || fullscreen);
        windowButton->setAccessibleName(windowButton->toolTip());
    }

    void openWindow(bool fullscreenMode)
    {
        if (!floating) {
            movingToWindow = true;
            // The window becomes the QObject parent of the complete player.
            // Reparenting does not recreate QMediaPlayer or reopen the stream.
            auto window = new VideoWindow(this, video, controls, [this] { closeWindow(); },
                                          [this] { leaveFullscreen(); });
            floating = window;
            window->resize(qMax(480, width()), qMax(320, height()));
            returnInlineOnFullscreenExit = fullscreenMode;
            if (fullscreenMode)
                window->setFullscreenMode(true);
            else
                window->show();
            movingToWindow = false;
        } else if (fullscreenMode) {
            returnInlineOnFullscreenExit = false;
            floating->setFullscreenMode(true);
        }
        if (floating) {
            floating->raise();
            floating->activateWindow();
        }
        updateFullscreenButton();
        updateWindowButton();
    }

    void enterFullscreen() { openWindow(true); }

    void leaveFullscreen()
    {
        if (!floating || !floating->isFullscreenMode())
            return;
        floating->setFullscreenMode(false);
        if (returnInlineOnFullscreenExit && inlineViewport) {
            dock();
            return;
        }
        returnInlineOnFullscreenExit = false;
        updateFullscreenButton();
        updateWindowButton();
    }

    void dock()
    {
        if (!floating || !inlineViewport)
            return;
        auto *window = floating.data();
        if (window->isFullscreenMode())
            window->setFullscreenMode(false);
        // Transfer ownership back to the chat before deleting the empty host.
        setParent(inlineViewport);
        floating = nullptr;
        returnInlineOnFullscreenExit = false;
        show();
        window->hide();
        window->deleteLater();
        updateFullscreenButton();
        updateWindowButton();
        if (onDocked)
            onDocked(); // Recompute viewport visibility and placeholder geometry.
    }

    void closeWindow()
    {
        if (!floating)
            return;
        if (inlineViewport) {
            dock();
            return;
        }
        auto *window = floating.data();
        if (window->isFullscreenMode())
            window->setFullscreenMode(false);
        window->hide();
        // With no message anchor the standalone window owns the player until
        // it closes; this releases decoder, media device and stream.
        window->deleteLater();
    }

    void updateState(bool playing)
    {
        button->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        button->setToolTip(playing ? ITEVideoController::tr("Pause") : ITEVideoController::tr("Play"));
        button->setAccessibleName(button->toolTip());
    }

    void sourceUnavailable()
    {
        // QObject emits destroyed before deleting its child devices. Release
        // the decoder first; the dying opener can no longer service close().
        delete player;
        player = nullptr;
        stream = nullptr;
        updateState(false);
        button->setEnabled(false);
        slider->setEnabled(false);
        if (downloadButton)
            downloadButton->setEnabled(false);
        time->setText(ITEVideoController::tr("Media source unavailable"));
        time->setToolTip(ITEVideoController::tr("The media source was closed by the application."));
    }

    void toggle()
    {
        if (requiresOpener && !opener)
            return;
        const bool retry = player
            && (player->error() != QMediaPlayer::NoError || (mediaState && !mediaState->sourceError().isEmpty()));
        if (player && (retry || (opener && player->mediaStatus() == QMediaPlayer::EndOfMedia))) {
            delete player;
            player = nullptr;
            if (stream && opener)
                opener->close(stream);
            stream = nullptr;
            source = originalSource;
            if (opener && retry)
                opener->retry(originalSource);
            slider->setEnabled(false);
            time->setText(QStringLiteral("0:00"));
            time->setToolTip({ });
        }
        if (!player) {
            player = new QMediaPlayer(this);
            video->attach(player);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            player->setAudioOutput(new QAudioOutput(player));
            connect(player, &QMediaPlayer::playbackStateChanged, this,
                    [this](QMediaPlayer::PlaybackState state) { updateState(state == QMediaPlayer::PlayingState); });
            connect(player, &QMediaPlayer::errorOccurred, this,
                    [this](QMediaPlayer::Error, const QString &message) { showError(message); });
#else
            connect(player, &QMediaPlayer::stateChanged, this,
                    [this](QMediaPlayer::State state) { updateState(state == QMediaPlayer::PlayingState); });
            connect(player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
                    [this](QMediaPlayer::Error) { showError(player->errorString()); });
#endif
            connect(player, &QMediaPlayer::seekableChanged, slider, &QSlider::setEnabled);
            connect(player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
                if (!slider->isSliderDown()) {
                    const QSignalBlocker blocker(slider);
                    slider->setValue(player->duration() > 0 ? int(position * 10000 / player->duration()) : 0);
                }
                time->setText(
                    QStringLiteral("%1:%2").arg(position / 60000).arg(position / 1000 % 60, 2, 10, QLatin1Char('0')));
            });
            stream = opener ? opener->open(source) : nullptr;
            if (opener)
                iteConfigureMediaNetworkTimeout(player, source);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (stream)
                player->setSourceDevice(stream, source);
            else
                player->setSource(source);
#else
            player->setMedia(source, stream);
#endif
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const bool playing = player->playbackState() == QMediaPlayer::PlayingState;
#else
        const bool playing = player->state() == QMediaPlayer::PlayingState;
#endif
        if (playing)
            player->pause();
        else
            player->play();
    }

    void showError(const QString &message)
    {
        const auto detail = mediaState && !mediaState->sourceError().isEmpty() ? mediaState->sourceError() : message;
        // Error UI is local too; never rewrite the document on backend events.
        time->setText(ITEVideoController::tr("Cannot play video"));
        time->setToolTip(detail);
        button->setToolTip(detail);
    }

    QUrl                         source;
    QUrl                         originalSource;
    QPointer<ITEMediaOpener>     opener;
    const bool                   requiresOpener;
    QPointer<QIODevice>          stream;
    QMediaPlayer                *player = nullptr;
    ITEVideoFrameWidget         *video;
    QVBoxLayout                 *layout;
    QWidget                     *controls;
    QToolButton                 *windowButton;
    QToolButton                 *fullscreenButton;
    QPointer<VideoWindow>        floating;
    QPointer<QWidget>            inlineViewport;
    QPointer<ITEVideoController> downloadController;
    std::function<void()>        onDocked;
    bool                         movingToWindow               = false;
    bool                         returnInlineOnFullscreenExit = false;
    QToolButton                 *button;
    ITEMediaSlider              *slider;
    QToolButton                 *downloadButton = nullptr;
    QPointer<ITEMediaState>      mediaState;
    QLabel                      *time;
};
}

class ITEVideoController::Private {
public:
    struct Entry {
        QTextCursor           cursor;
        QPointer<VideoPlayer> widget;
    };
    ITEVideoController  *q;
    QMap<quint32, Entry> entries;
    bool                 pending = false;

    explicit Private(ITEVideoController *controller) : q(controller) { }
    ~Private()
    {
        for (const auto &entry : entries) {
            if (entry.widget) {
                if (entry.widget->isFloating())
                    entry.widget->orphan(); // Window continues after the chat is gone.
                else
                    delete entry.widget;
            }
        }
    }

    void schedule()
    {
        if (pending)
            return;
        pending = true;
        QTimer::singleShot(0, q, [this] {
            pending = false;
            sync();
        });
    }

    void sync()
    {
        if (!q->itc || !q->itc->textEdit())
            return;
        auto         edit = q->itc->textEdit();
        const QPoint offset(edit->horizontalScrollBar()->value(), edit->verticalScrollBar()->value());
        for (auto it = entries.begin(); it != entries.end();) {
            auto &entry = it.value();
            if (entry.cursor.document() != edit->document()
                || entry.cursor.selectedText() != QString(QChar::ObjectReplacementCharacter)
                || entry.cursor.charFormat().objectType() != q->objectType
                || InteractiveTextFormat::id(entry.cursor.charFormat()) != it.key()) {
                if (entry.widget) {
                    if (entry.widget->isFloating())
                        entry.widget->orphan();
                    else
                        delete entry.widget;
                }
                it = entries.erase(it);
                continue;
            }
            const QRect rect    = q->itc->elementRect(entry.cursor).translated(-offset);
            const bool  visible = rect.isValid() && rect.intersects(edit->viewport()->rect());
            if (visible && !entry.widget) {
                entry.widget = new VideoPlayer(entry.cursor.charFormat(), edit->viewport(), q, [this] { schedule(); });
            }
            if (entry.widget && !entry.widget->isFloating()) {
                // Never hide/reparent an undocked player as the chat scrolls.
                entry.widget->setGeometry(rect);
                entry.widget->setVisible(visible);
            }
            ++it;
        }
    }
};

ITEVideoController::ITEVideoController(InteractiveText *itc, QObject *parent) :
    InteractiveTextElementController(itc, parent), d(new Private(this))
{
    itc->textEdit()->viewport()->installEventFilter(this);
    connect(itc, &InteractiveText::viewportChanged, this, [this] { d->schedule(); });
    connect(itc->textEdit()->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this] { d->schedule(); });
    connect(itc->textEdit()->document(), &QTextDocument::contentsChanged, this, [this] { d->schedule(); });
}

ITEVideoController::~ITEVideoController() = default;

QTextCharFormat ITEVideoController::makeFormat(const QUrl &source, ITEMediaOpener *opener, const QSize &size,
                                               bool downloadable) const
{
    InteractiveTextFormat format(objectType, itc->nextId());
    format.setProperty(Source, source);
    if (opener)
        format.setProperty(Opener, QVariant::fromValue(QPointer<ITEMediaOpener>(opener)));
    format.setProperty(Size, size.expandedTo(QSize(160, 120)));
    format.setProperty(Downloadable, downloadable);
    format.setVerticalAlignment(QTextCharFormat::AlignNormal);
    return format;
}

void ITEVideoController::insert(const QUrl &source, ITEMediaOpener *opener, const QSize &size, bool downloadable)
{
    itc->insert(InteractiveTextFormat(makeFormat(source, opener, size, downloadable)));
}

QSizeF ITEVideoController::intrinsicSize(QTextDocument *, int, const QTextFormat &format)
{
    return format.property(Size).toSize();
}

void ITEVideoController::drawITE(QPainter *painter, const QRectF &rect, int position, const QTextFormat &format)
{
    // Fallback for printing/initial exposure. Widgets are created after painting,
    // avoiding document/layout reentry. Frames never call this function.
    painter->fillRect(rect, Qt::black);
    painter->save();
    painter->setPen(Qt::white);
    painter->drawText(rect, Qt::AlignCenter, tr("Video"));
    painter->restore();
    auto id = InteractiveTextFormat::id(format);
    if (!d->entries.contains(id)) {
        QTextCursor cursor(itc->textEdit()->document());
        cursor.setPosition(position);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        d->entries.insert(id, { cursor, nullptr });
        d->schedule();
    }
}

bool ITEVideoController::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
        d->schedule();
    return false;
}
