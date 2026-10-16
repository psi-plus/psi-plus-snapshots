/* SPDX-License-Identifier: Apache-2.0 */
#include "qitevideo.h"
#include "qitemedia_p.h"
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

class FullscreenVideo : public QWidget {
public:
    FullscreenVideo(QWidget *owner, QWidget *frame, QWidget *controls, std::function<void()> exit) :
        QWidget(owner->window(), Qt::Window), frame(frame), controls(controls), exit(std::move(exit))
    {
        setObjectName(QStringLiteral("qite-video-fullscreen"));
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMouseTracking(true);
        frame->setParent(this);
        controls->setParent(this);
        const auto widgets = findChildren<QWidget *>();
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
        connect(escape, &QShortcut::activated, this, [this] { requestExit(); });
        frame->show();
        showControls();
    }

    void deactivate()
    {
        active = false;
        hideTimer.stop();
        exit = {};
        frame->unsetCursor();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }
    void resizeEvent(QResizeEvent *) override
    {
        if (!active)
            return;
        frame->setGeometry(rect());
        const int height = controls->sizeHint().height();
        controls->setGeometry(12, qMax(0, this->height() - height - 12), qMax(0, width() - 24), height);
        controls->raise();
    }
    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();
        requestExit();
    }
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() != QEvent::WindowStateChange || !active)
            return;
        if (isFullScreen()) {
            wasFullscreen = true;
        } else if (wasFullscreen && !isMinimized()) {
            // A window-manager action can leave fullscreen independently of
            // our button. Restore inline after the native state change ends.
            QTimer::singleShot(0, this, [this] {
                if (active && !isFullScreen() && !isMinimized())
                    requestExit();
            });
        }
    }
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (active
            && (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter
                || event->type() == QEvent::KeyPress))
            showControls();
        return false;
    }
    void mouseMoveEvent(QMouseEvent *) override { showControls(); }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            event->accept();
            requestExit();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    void showControls()
    {
        if (!active)
            return;
        frame->unsetCursor();
        setCursor(Qt::ArrowCursor);
        controls->show();
        controls->raise();
        hideTimer.start();
    }
    void requestExit()
    {
        const auto callback = exit;
        if (callback)
            callback();
    }

    QWidget              *frame;
    QWidget              *controls;
    std::function<void()> exit;
    QTimer                hideTimer;
    bool                  active        = true;
    bool                  wasFullscreen = false;
};

class VideoPlayer : public QWidget {
public:
    VideoPlayer(const QTextFormat &format, QWidget *parent, std::function<void()> download) :
        QWidget(parent), source(format.property(Source).toUrl()),
        opener(static_cast<ITEMediaOpener *>(format.property(Opener).value<void *>()))
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
            connect(downloadButton, &QToolButton::clicked, this, [download] { download(); });
            auto updateDownload = [this] {
                downloadButton->setEnabled(!mediaState || !mediaState->downloading());
                downloadButton->setToolTip(iteMediaDownloadText(mediaState));
                downloadButton->setAccessibleName(downloadButton->toolTip());
            };
            if (mediaState) {
                connect(mediaState, &ITEMediaState::changed, this, updateDownload);
                connect(mediaState, &QObject::destroyed, this, updateDownload);
            }
            updateDownload();
        }
        fullscreenButton = new QToolButton(controls);
        fullscreenButton->setObjectName(QStringLiteral("qite-video-fullscreen-button"));
        updateFullscreenButton();
        row->addWidget(fullscreenButton);
        layout->addWidget(controls);
        connect(fullscreenButton, &QToolButton::clicked, this, [this] {
            if (fullscreen)
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
    }

    ~VideoPlayer() override
    {
        leaveFullscreen();
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

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }
    void hideEvent(QHideEvent *event) override
    {
        if (!fullscreen)
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
        fullscreenButton->setIcon(
            style()->standardIcon(fullscreen ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
        fullscreenButton->setToolTip(fullscreen ? ITEVideoController::tr("Exit full screen")
                                                : ITEVideoController::tr("Full screen"));
        fullscreenButton->setAccessibleName(fullscreenButton->toolTip());
    }
    void enterFullscreen()
    {
        layout->removeWidget(video);
        layout->removeWidget(controls);
        fullscreen = new FullscreenVideo(this, video, controls, [this] { leaveFullscreen(); });
        updateFullscreenButton();
        fullscreen->showFullScreen();
        fullscreen->activateWindow();
        fullscreen->setFocus();
    }
    void leaveFullscreen()
    {
        if (!fullscreen)
            return;
        auto window = fullscreen.data();
        fullscreen  = nullptr;
        window->deactivate();
        video->setParent(this);
        controls->setParent(this);
        layout->addWidget(video, 1);
        layout->addWidget(controls);
        video->show();
        controls->show();
        updateFullscreenButton();
        window->hide();
        window->deleteLater();
        if (!isVisible())
            pause();
    }

    void updateState(bool playing)
    {
        button->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        button->setToolTip(playing ? ITEVideoController::tr("Pause") : ITEVideoController::tr("Play"));
        button->setAccessibleName(button->toolTip());
    }

    void toggle()
    {
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
        // Error UI is local too; never rewrite the document on backend events.
        time->setText(ITEVideoController::tr("Cannot play video"));
        time->setToolTip(message);
        button->setToolTip(message);
    }

    QUrl                      source;
    ITEMediaOpener           *opener;
    QPointer<QIODevice>       stream;
    QMediaPlayer             *player = nullptr;
    ITEVideoFrameWidget      *video;
    QVBoxLayout              *layout;
    QWidget                  *controls;
    QToolButton              *fullscreenButton;
    QPointer<FullscreenVideo> fullscreen;
    QToolButton              *button;
    ITEMediaSlider           *slider;
    QToolButton              *downloadButton = nullptr;
    QPointer<ITEMediaState>   mediaState;
    QLabel                   *time;
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
        for (const auto &entry : entries)
            delete entry.widget;
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
                delete entry.widget;
                it = entries.erase(it);
                continue;
            }
            const QRect rect    = q->itc->elementRect(entry.cursor).translated(-offset);
            const bool  visible = rect.isValid() && rect.intersects(edit->viewport()->rect());
            if (visible && !entry.widget) {
                const auto source = entry.cursor.charFormat().property(Source).toUrl();
                entry.widget = new VideoPlayer(entry.cursor.charFormat(), edit->viewport(), [controller = q, source] {
                    emit controller->downloadRequested(source);
                });
            }
            if (entry.widget) {
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
    format.setProperty(Opener, QVariant::fromValue<void *>(opener));
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
