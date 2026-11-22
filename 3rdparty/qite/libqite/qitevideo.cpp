/* SPDX-License-Identifier: Apache-2.0 */
#include "qitevideo.h"
#include "qitemedia_p.h"
#include "qitemediaplayer_p.h"
#include "qitevideoframe_p.h"

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QSizePolicy>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif
#include <QCloseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include <QMouseEvent>
#include <QImage>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <functional>
#include <utility>

namespace {
enum Property { Source = InteractiveTextFormat::UserProperty, Opener, Size, Downloadable };

QString mediaClock(qint64 millis)
{
    const qint64 seconds = qMax<qint64>(0, millis) / 1000;
    if (seconds >= 3600)
        return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}


// Qite always paints the transport controls on black. Preserve each style
// icon's native resolution and mode/state while using a legible foreground.
// Recolor at the requested device scale, rather than capturing a 24px bitmap.
class VideoControlIconEngine final : public QIconEngine {
public:
    explicit VideoControlIconEngine(QIcon source) : source_(std::move(source)) {}

    QIconEngine *clone() const override { return new VideoControlIconEngine(source_); }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool isNull() override { return source_.isNull(); }
#endif

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        if (!painter || rect.isEmpty())
            return;
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const QPixmap rendered = renderPixmap(rect.size(), mode, state, dpr);
        if (!rendered.isNull())
            painter->drawPixmap(rect, rendered);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return renderPixmap(size, mode, state, 1.0);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal dpr) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
        // Before Qt 6.8, Qt incorrectly supplied physical pixels as size.
        const QSize logical(qMax(1, qRound(size.width() / qMax<qreal>(1.0, dpr))),
                            qMax(1, qRound(size.height() / qMax<qreal>(1.0, dpr))));
        return renderPixmap(logical, mode, state, dpr);
#else
        return renderPixmap(size, mode, state, dpr);
#endif
    }
#else
    // In Qt 5 isNull() and scaledPixmap() are nonvirtual wrappers around
    // virtual_hook(); overriding those methods has no effect (and does not
    // compile when marked override).
    void virtual_hook(int id, void *data) override
    {
        if (id == QIconEngine::IsNullHook) {
            if (data)
                *static_cast<bool *>(data) = source_.isNull();
            return;
        }
        if (id == QIconEngine::ScaledPixmapHook) {
            if (data) {
                auto *args = static_cast<QIconEngine::ScaledPixmapArgument *>(data);
                args->pixmap = renderPixmap(args->size, args->mode, args->state, args->scale);
            }
            return;
        }
        QIconEngine::virtual_hook(id, data);
    }
#endif

private:
    QPixmap renderPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal dpr) const
    {
        if (size.isEmpty())
            return {};
        dpr = qMax<qreal>(1.0, dpr);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPixmap image = source_.pixmap(size, dpr, mode, state);
#else
        // Qt 5 has no QIcon::pixmap(QSize, qreal, Mode, State) overload.
        // Request the physical pixels, retaining the logical icon dimensions.
        const QSize pixels(qMax(1, qRound(size.width() * dpr)),
                           qMax(1, qRound(size.height() * dpr)));
        QPixmap image = source_.pixmap(pixels, mode, state);
        if (!image.isNull())
            image.setDevicePixelRatio(dpr);
#endif
        if (image.isNull())
            return image;
        QPainter tint(&image);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(QRect(QPoint(0, 0), image.size()), QColor(235, 240, 248));
        return image;
    }

    QIcon source_;
};

QIcon videoControlIcon(const QIcon &original)
{
    return original.isNull() ? original : QIcon(new VideoControlIconEngine(original));
}

class VideoControls : public QWidget {
public:
    using Visibility = ITEVideoController::ControlVisibility;
    struct Control {
        QString id;
        QPointer<QToolButton> button;
        Visibility visibility;
        bool available = true;
    };

    explicit VideoControls(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setStyleSheet(QStringLiteral(
            "QToolButton { border: 0; padding: 3px; background: transparent; }"
            "QToolButton:hover:enabled { background: rgba(255,255,255,30); border-radius: 4px; }"
            "QToolButton:pressed:enabled { background: rgba(255,255,255,55); border-radius: 4px; }"
            "QToolButton:focus { border: 1px solid rgba(255,255,255,105); padding: 2px;"
            " border-radius: 4px; }"));
    }

    void registerButton(QToolButton *button, const QString &id, Visibility initial, const QString &afterId = {})
    {
        if (!button || id.isEmpty() || findControl(id))
            return;
        const Control entry { id, button, initial, true };
        int insertAt = controls_.size();
        for (int i = 0; i < controls_.size(); ++i) {
            if (!afterId.isEmpty() && controls_.at(i).id == afterId) {
                insertAt = i + 1;
                break;
            }
        }
        controls_.insert(insertAt, entry);
        refresh();
    }

    void complete()
    {
        auto *row = qobject_cast<QHBoxLayout *>(layout());
        if (!row)
            return;
        more_ = new QToolButton(this);
        more_->setObjectName(QStringLiteral("qite-video-more-button"));
        more_->setText(QStringLiteral("⋮"));
        more_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // InstantPopup adds a native menu arrow on some Qt styles. The
        // vertical ellipsis already communicates that this opens a menu.
        more_->setStyleSheet(QStringLiteral(
            "QToolButton::menu-indicator { image: none; width: 0px; height: 0px; }"));
        more_->setToolTip(tr("More controls"));
        more_->setAccessibleName(more_->toolTip());
        more_->setFocusPolicy(Qt::TabFocus);
        more_->setAutoRaise(true);
        more_->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(more_);
        more_->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] { populateMenu(menu); });
        row->addWidget(more_);
        more_->hide();
        refresh();
    }

    QToolButton *addPlayerAction(const QString &id, QAction *action, const QString &afterId,
                                 Visibility initial)
    {
        if (!action || id.isEmpty() || findControl(id))
            return nullptr;
        auto *row = qobject_cast<QHBoxLayout *>(layout());
        if (!row)
            return nullptr;
        auto *button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::TabFocus);
        button->setObjectName(id);
        button->setDefaultAction(action);
        // Host icons may be designed for light toolbars. Apply the same
        // high-contrast rendering as the built-in media controls.
        button->setIcon(videoControlIcon(action->icon()));
        connect(action, &QAction::changed, button, [button, action] {
            button->setIcon(videoControlIcon(action->icon()));
        });
        int index = more_ ? row->indexOf(more_) : row->count();
        if (const auto *after = findControl(afterId))
            if (after->button)
                index = row->indexOf(after->button) + 1;
        row->insertWidget(index, button);
        registerButton(button, id, initial, afterId);
        connect(action, &QObject::destroyed, this, [this, button, id] {
            for (int i = 0; i < controls_.size(); ++i) {
                if (controls_.at(i).id == id && controls_.at(i).button == button) {
                    controls_.removeAt(i);
                    break;
                }
            }
            button->deleteLater();
            refresh();
        });
        return button;
    }

    bool setVisibility(const QString &id, Visibility visibility)
    {
        auto *control = findControl(id);
        if (!control)
            return false;
        control->visibility = visibility;
        refresh();
        return true;
    }

    void setAvailable(const QString &id, bool available)
    {
        if (auto *control = findControl(id)) {
            control->available = available;
            refresh();
        }
    }

    void refresh()
    {
        if (refreshing_ || !more_)
            return;
        refreshing_ = true;
        // Calculate the target layout before changing any child visibility.
        // Re-showing and re-hiding auto buttons on every layout request would
        // create an endless resize/layout-request cycle.
        QVector<char> direct(controls_.size(), false);
        bool menuNeeded = false;
        for (int i = 0; i < controls_.size(); ++i) {
            const auto &control = controls_.at(i);
            direct[i] = control.button && control.available
                && control.visibility != Visibility::Overflow;
            menuNeeded |= control.button && control.available && !direct.at(i);
        }
        if (!fits(direct, menuNeeded)) {
            auto *row = qobject_cast<QHBoxLayout *>(layout());
            // Evict only Automatic actions, rightmost first. Explicitly
            // pinned actions are never forced into the overflow menu.
            for (int i = row->count() - 1; i >= 0; --i) {
                const auto *button = row->itemAt(i)->widget();
                for (int n = 0; n < controls_.size(); ++n) {
                    if (controls_.at(n).button == button && direct.at(n)
                        && controls_.at(n).visibility == Visibility::Automatic) {
                        direct[n] = false;
                        menuNeeded = true;
                        break;
                    }
                }
                if (fits(direct, menuNeeded))
                    break;
            }
        }
        for (int i = 0; i < controls_.size(); ++i) {
            auto *button = controls_.at(i).button.data();
            if (button && button->isHidden() == bool(direct.at(i)))
                button->setVisible(bool(direct.at(i)));
        }
        if (more_->isHidden() == menuNeeded)
            more_->setVisible(menuNeeded);
        refreshing_ = false;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        refresh();
    }
    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest)
            refresh(); // Also handles cache-indicator and buffering visibility.
        return handled;
    }
private:
    Control *findControl(const QString &id)
    {
        for (auto &entry : controls_)
            if (entry.id == id)
                return &entry;
        return nullptr;
    }
    const Control *findControl(const QString &id) const
    {
        for (const auto &entry : controls_)
            if (entry.id == id)
                return &entry;
        return nullptr;
    }

    bool fits(const QVector<char> &direct, bool menuVisible) const
    {
        // The Play button lives to the left of the seek slider and is not
        // part of the right-hand action group. That group may display up to
        // three buttons, or two buttons and the overflow trigger. Pinned
        // actions are never evicted even if the host exceeds this limit.
        int actionSlots = menuVisible ? 1 : 0;
        for (int i = 0; i < controls_.size(); ++i) {
            if (direct.at(i) && controls_.at(i).id != QLatin1String("qite-video-play"))
                ++actionSlots;
        }
        if (actionSlots > 3)
            return false;

        auto *row = qobject_cast<QHBoxLayout *>(layout());
        if (!row || width() <= 0)
            return true;
        const auto margins = row->contentsMargins();
        int needed = margins.left() + margins.right(), count = 0;
        for (int i = 0; i < row->count(); ++i) {
            const auto *widget = row->itemAt(i)->widget();
            if (!widget)
                continue;
            bool visible = widget == more_ ? menuVisible : !widget->isHidden();
            for (int n = 0; n < controls_.size(); ++n) {
                if (controls_.at(n).button == widget) {
                    visible = direct.at(n);
                    break;
                }
            }
            if (!visible)
                continue;
            needed += qobject_cast<const QSlider *>(widget) ? qMax(64, widget->minimumWidth())
                                                             : widget->sizeHint().width();
            ++count;
        }
        return needed + qMax(0, count - 1) * row->spacing() <= width();
    }

    void populateMenu(QMenu *menu)
    {
        menu->clear();
        for (const auto &control : controls_) {
            if (!control.available || !control.button || !control.button->isHidden())
                continue;
            const QPointer<QToolButton> button(control.button);
            const QString title = button->toolTip().isEmpty() ? control.id : button->toolTip();
            auto *item = menu->addAction(button->icon(), title, this, [button] {
                if (button)
                    button->click();
            });
            item->setEnabled(button->isEnabled());
        }
    }

    QList<Control> controls_;
    QToolButton *more_ = nullptr;
    bool refreshing_ = false;
};

// Cover art is supplied by QMediaPlayer metadata after its range-backed
// source becomes available. Until then, draw a small music-note glyph without
// attempting to read or download the complete audio file.
class AudioArtworkWidget final : public QWidget {
public:
    explicit AudioArtworkWidget(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("qite-audio-artwork"));
        setMinimumHeight(90);
    }

    void setCover(QImage cover)
    {
        cover_ = std::move(cover);
        update();
    }

    bool hasCover() const { return !cover_.isNull(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!cover_.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const QSize size = cover_.size().scaled(rect().adjusted(20, 12, -20, -12).size(),
                                                   Qt::KeepAspectRatio);
            const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
            painter.drawImage(target, cover_);
            return;
        }
        painter.translate(width() / 2.0, height() / 2.0);
        const qreal scale = qMin(width() / 90.0, height() / 90.0);
        painter.scale(scale, scale);
        const QColor ink(190, 198, 211);
        painter.setPen(QPen(ink, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(ink);
        painter.drawEllipse(QRectF(-22, 12, 14, 9));
        painter.drawEllipse(QRectF(3, 6, 14, 9));
        painter.drawLine(QPointF(-8, 17), QPointF(-8, -17));
        painter.drawLine(QPointF(17, 11), QPointF(17, -22));
        painter.drawLine(QPointF(-8, -17), QPointF(17, -22));
    }

private:
    QImage cover_;
};

// This top-level window owns the VideoPlayer while it is undocked. The chat
// controller holds only a weak pointer, so chat-log pruning cannot stop playback.
class VideoWindow : public QWidget {
public:
    VideoWindow(QWidget *owner, QWidget *frame, QWidget *controls, std::function<void()> close,
                std::function<void()> exitFullscreen, bool audioPresentation) :
        QWidget(nullptr, Qt::Window), owner(owner), frame(frame), controls(controls), closeAction(std::move(close)),
        exitFullscreenAction(std::move(exitFullscreen))
    {
        setObjectName(QStringLiteral("qite-video-window"));
        setWindowTitle(audioPresentation ? ITEVideoController::tr("Audio") : ITEVideoController::tr("Video"));
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
                std::function<void()> docked, bool audioPresentation = false, bool compactInlineAudio = false) :
        QWidget(parent), source(format.property(Source).toUrl()), originalSource(source),
        opener(format.property(Opener).value<QPointer<ITEMediaOpener>>()), requiresOpener(format.hasProperty(Opener)),
        compactInlineAudio_(audioPresentation && compactInlineAudio),
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
        palette.setColor(QPalette::ButtonText, Qt::white);
        setPalette(palette);
        layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        video = new ITEVideoFrameWidget(this);
        video->setCursor(Qt::PointingHandCursor);
        video->installEventFilter(this);
        layout->addWidget(video, 1);
        if (audioPresentation) {
            artwork = new AudioArtworkWidget(this);
            artwork->setCursor(Qt::PointingHandCursor);
            artwork->installEventFilter(this);
            layout->insertWidget(0, artwork, 1);
            video->hide();
            updateAudioArtworkVisibility();
        }
        controls = new VideoControls(this);
        controls->setObjectName(QStringLiteral("qite-video-controls"));
        controls->setPalette(palette);
        auto row = new QHBoxLayout(controls);
        row->setContentsMargins(6, 4, 6, 4);
        button = new QToolButton(controls);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::TabFocus);
        button->setObjectName(QStringLiteral("qite-video-play"));
        button->setIcon(videoControlIcon(style()->standardIcon(QStyle::SP_MediaPlay)));
        button->setToolTip(ITEVideoController::tr("Play"));
        button->setAccessibleName(button->toolTip());
        row->addWidget(button);
        controls->registerButton(button, QStringLiteral("qite-video-play"),
                                 ITEVideoController::ControlVisibility::AlwaysVisible);
        slider     = new ITEMediaSlider(controls);
        mediaState = opener ? opener->mediaState(source) : nullptr;
        if (mediaState)
            connect(mediaState, &ITEMediaState::changed, this, [this] {
                if (player && mediaState && !mediaState->sourceError().isEmpty()) {
                    static_cast<ITEMediaPlayer *>(player)->abortInput();
                    player->stop();
                    showError(mediaState->sourceError());
                }
            });
        slider->setMediaState(mediaState);
        slider->setRange(0, 10000);
        slider->setEnabled(false);
        slider->setAccessibleName(ITEVideoController::tr("Video position"));
        row->addWidget(slider, 1);
        time = new QLabel(QStringLiteral("–:––"), controls);
        time->setObjectName(QStringLiteral("qite-video-duration"));
        // A QTextEdit stylesheet may override child palettes. Give the time an
        // explicit foreground on our opaque black background as well.
        time->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
        row->addWidget(time);
        // Downloaded file bytes are distinct from video playback position.
        // The host provides this state without Qite knowing its transport.
        transferIndicator = iteCreateMediaTransferIndicator(mediaState, controls);
        row->addWidget(transferIndicator);
        buffering = new QLabel(ITEVideoController::tr("Loading…"), controls);
        buffering->setObjectName(QStringLiteral("qite-video-loading"));
        buffering->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
        buffering->hide();
        row->addWidget(buffering);
        if (format.boolProperty(Downloadable)) {
            downloadButton = new QToolButton(controls);
            downloadButton->setAutoRaise(true);
            downloadButton->setFocusPolicy(Qt::TabFocus);
            downloadButton->setObjectName(QStringLiteral("qite-video-download"));
            downloadButton->setIcon(
                QIcon::fromTheme(QStringLiteral("document-save"), style()->standardIcon(QStyle::SP_DialogSaveButton)));
            row->addWidget(downloadButton);
            controls->registerButton(downloadButton, QStringLiteral("qite-video-download"),
                                     ITEVideoController::ControlVisibility::Automatic);
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
        windowButton->setAutoRaise(true);
        windowButton->setFocusPolicy(Qt::TabFocus);
        windowButton->setObjectName(QStringLiteral("qite-video-window-button"));
        windowButton->setIcon(videoControlIcon(
            QIcon::fromTheme(QStringLiteral("window-new"), style()->standardIcon(QStyle::SP_TitleBarNormalButton))));
        updateWindowButton();
        row->addWidget(windowButton);
        controls->registerButton(windowButton, QStringLiteral("qite-video-window-button"),
                                 ITEVideoController::ControlVisibility::Automatic);
        fullscreenButton = new QToolButton(controls);
        fullscreenButton->setAutoRaise(true);
        fullscreenButton->setFocusPolicy(Qt::TabFocus);
        fullscreenButton->setObjectName(QStringLiteral("qite-video-fullscreen-button"));
        updateFullscreenButton();
        row->addWidget(fullscreenButton);
        controls->registerButton(fullscreenButton, QStringLiteral("qite-video-fullscreen-button"),
                                 ITEVideoController::ControlVisibility::AlwaysVisible);
        controls->complete();
        if (opener)
            opener->populatePlayerActions(originalSource, this);
        layout->addWidget(controls);
        connect(windowButton, &QToolButton::clicked, this, [this] {
            const QPointer<VideoPlayer> guard(this);
            if (transferToHost(false))
                return;
            if (downloadController
                && downloadController->requestWindow(originalSource, player ? player->position() : 0, false)) {
                if (guard)
                    pause();
                return;
            }
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
            if (externalFullscreen) {
                externalFullscreen();
                updateFullscreenButton();
                return;
            }
            const QPointer<VideoPlayer> guard(this);
            if (transferToHost(true))
                return;
            if (downloadController
                && downloadController->requestWindow(originalSource, player ? player->position() : 0, true)) {
                if (guard)
                    pause();
                return;
            }
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
    }

    void pause()
    {
        if (player)
            player->pause();
    }

    bool isFloating() const { return !floating.isNull() || hostedExternally_; }
    void setHostFullscreen(std::function<void()> action)
    {
        externalFullscreen = std::move(action);
        // Only a host-owned viewer (e.g. Psi's MediaViewer) replaces these
        // controls. A standalone player created without a host handler still
        // needs its own pop-out and fullscreen buttons.
        controls->setAvailable(QStringLiteral("qite-video-window-button"), !externalFullscreen);
        controls->setAvailable(QStringLiteral("qite-video-fullscreen-button"), !externalFullscreen);
    }
    void startPlayback() { toggle(); }
    void setInitialPosition(qint64 position) { initialPosition = std::max<qint64>(0, position); }

    void orphan()
    {
        // Called when the document loses its attachment or its controller is
        // destroyed. A detached window remains the player's sole owner.
        inlineViewport               = nullptr;
        onDocked                     = {};
        returnInlineOnFullscreenExit = false;
        updateAudioArtworkVisibility();
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
        if (!floating && !externalFullscreen && !movingToWindow && !hostedExternally_)
            pause();
        QWidget::hideEvent(event);
    }
    bool eventFilter(QObject *object, QEvent *event) override
    {
        // Artwork is the visible audio frame and has the same playback hit
        // target as video, including when a fallback note is displayed.
        if (object == video || object == artwork) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                fullscreenButton->click();
                return true;
            }
            if (event->type() == QEvent::MouseButtonRelease
                && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                toggle();
                return true;
            }
        }
        return false;
    }

private:
    void updateAudioArtworkVisibility()
    {
        if (!artwork)
            return;
        const bool visible = !compactInlineAudio_ || artwork->hasCover()
            || hostedExternally_ || floating || !inlineViewport;
        if (artwork->isHidden() == visible)
            artwork->setVisible(visible);
        // Only a state transition notifies the host, after the new layout
        // visibility is applied. No decoder restart or deferred resize.
        if (property("qite-audio-artwork-visible").toBool() != visible) {
            setProperty("qite-audio-artwork-visible", visible);
            updateGeometry();
        }
    }

    bool transferToHost(bool fullscreen)
    {
        if (!downloadController || hostedExternally_)
            return false;
        // Reparenting can emit hideEvent and the chat's layout can receive
        // synchronous update signals. Keep the decoder running in both cases.
        hostedExternally_ = true;
        movingToWindow = true;
        const QPointer<VideoPlayer> guard(this);
        const bool transferred = downloadController->requestPlayerTransfer(
            originalSource, this, [guard] { return guard && guard->restoreFromHost(); }, fullscreen);
        if (!guard)
            return transferred;
        movingToWindow = false;
        if (!transferred) {
            hostedExternally_ = false;
            return false;
        }
        controls->setAvailable(QStringLiteral("qite-video-window-button"), false);
        controls->setAvailable(QStringLiteral("qite-video-fullscreen-button"), false);
        updateAudioArtworkVisibility();
        return true;
    }

    bool restoreFromHost()
    {
        if (!hostedExternally_ || !inlineViewport || !downloadController)
            return false;
        movingToWindow = true;
        hostedExternally_ = false;
        setParent(inlineViewport);
        updateAudioArtworkVisibility();
        controls->setAvailable(QStringLiteral("qite-video-window-button"), !externalFullscreen);
        controls->setAvailable(QStringLiteral("qite-video-fullscreen-button"), !externalFullscreen);
        updateFullscreenButton();
        updateWindowButton();
        show();
        movingToWindow = false;
        if (onDocked)
            onDocked(); // Rebind to the original chat's current viewport row.
        return true;
    }

    void updateFullscreenButton()
    {
        const bool fullscreen
            = externalFullscreen ? window()->isFullScreen() : (floating && floating->isFullscreenMode());
        fullscreenButton->setIcon(videoControlIcon(ITEVideoController::fullscreenIcon(palette(), fullscreen)));
        fullscreenButton->setToolTip(fullscreen ? ITEVideoController::tr("Exit full screen")
                                                : ITEVideoController::tr("Full screen"));
        fullscreenButton->setAccessibleName(fullscreenButton->toolTip());
    }

    void updateWindowButton()
    {
        const bool fullscreen = floating && floating->isFullscreenMode();
        windowButton->setToolTip(fullscreen                       ? ITEVideoController::tr("Open in separate window")
                                     : floating && inlineViewport ? ITEVideoController::tr("Return to chat")
                                     : floating                   ? (artwork ? ITEVideoController::tr("Audio window")
                                                                            : ITEVideoController::tr("Video window"))
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
            // For audio, artwork is the actual visual content. Moving the
            // hidden video frame into fullscreen would produce a black screen
            // instead of the cover (or fallback note).
            QWidget *visual = artwork ? static_cast<QWidget *>(artwork) : static_cast<QWidget *>(video);
            auto window
                = new VideoWindow(this, visual, controls, [this] { closeWindow(); },
                                  [this] { leaveFullscreen(); }, artwork != nullptr);
            floating = window;
            updateAudioArtworkVisibility();
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
        floating                     = nullptr;
        returnInlineOnFullscreenExit = false;
        updateAudioArtworkVisibility();
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
        button->setIcon(videoControlIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay)));
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
        // An already-playing stream must pause without entering error recovery.
        // A video-decoder error may coexist with working audio playback.
        if (player) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const auto playbackState = player->playbackState();
#else
            const auto playbackState = player->state();
#endif
            if (playbackState == QMediaPlayer::PlayingState) {
                player->pause();
                return;
            }
            if (playbackState == QMediaPlayer::PausedState) {
                player->play();
                return;
            }
        }
        const bool retry = player
            && (inputFailed || player->error() != QMediaPlayer::NoError
                || (mediaState && !mediaState->sourceError().isEmpty()));
        if (player && (retry || (opener && player->mediaStatus() == QMediaPlayer::EndOfMedia))) {
            delete player;
            player = nullptr;
            stream = nullptr;
            if (artwork) {
                artwork->setCover({});
                updateAudioArtworkVisibility();
            }
            source = originalSource;
            if (opener && retry)
                opener->retry(originalSource);
            slider->setEnabled(false);
            updateTime();
            time->setToolTip({});
        }
        if (!player) {
            inputFailed = false;
            player = new ITEMediaPlayer(this);
            video->attach(player);
            if (artwork)
                connect(player, QOverload<>::of(&QMediaPlayer::metaDataChanged), this,
                        [this] { updateCoverArt(); });
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
            connect(player, &QMediaPlayer::seekableChanged, this, [this](bool seekable) {
                slider->setEnabled(seekable);
                if (seekable && initialPosition) {
                    player->setPosition(initialPosition);
                    initialPosition = 0;
                }
            });
            connect(player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
                buffering->setVisible(status == QMediaPlayer::LoadingMedia || status == QMediaPlayer::BufferingMedia
                                      || status == QMediaPlayer::StalledMedia);
                if (initialPosition && player->isSeekable()
                    && (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)) {
                    player->setPosition(initialPosition);
                    initialPosition = 0;
                }
            });
            connect(player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
                if (!slider->isSliderDown()) {
                    const QSignalBlocker blocker(slider);
                    slider->setValue(player->duration() > 0 ? int(position * 10000 / player->duration()) : 0);
                }
                updateTime();
            });
            connect(player, &QMediaPlayer::durationChanged, this, [this](qint64) { updateTime(); });
            if (opener) {
                buffering->show();
                button->setEnabled(false);
                const QPointer<VideoPlayer>  guard(this);
                const QPointer<QMediaPlayer> expected(player);
                auto                         cancel = opener->openAsync(source, player,
                                                                        [guard, expected](QIODevice *input, QUrl resolved, QString error) {
                                                    if (!guard || !expected || guard->player != expected)
                                                        return;
                                                    guard->button->setEnabled(true);
                                                    if (!error.isEmpty()) {
                                                        guard->showError(error);
                                                        return;
                                                    }
                                                    guard->attachInput(input, resolved);
                                                    guard->player->play();
                                                });
                static_cast<ITEMediaPlayer *>(player)->setPendingOpen(std::move(cancel));
                return;
            }
            attachInput(nullptr, source);
        }
        player->play();
    }

    void updateTime()
    {
        const qint64 duration = player ? player->duration() : 0;
        const qint64 position = player ? player->position() : 0;
        time->setText(duration > 0 ? mediaClock(duration) : QStringLiteral("–:––"));
        slider->setMediaDuration(duration);
    }

    void updateCoverArt()
    {
        if (!artwork || !player)
            return;
        const auto asImage = [](const QVariant &value) {
            QImage image = value.value<QImage>();
            if (image.isNull() && value.canConvert<QPixmap>())
                image = value.value<QPixmap>().toImage();
            return image;
        };
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const auto meta = player->metaData();
        QImage cover = asImage(meta.value(QMediaMetaData::CoverArtImage));
#if QT_VERSION < QT_VERSION_CHECK(6, 12, 0)
        // Qt 6.12 deprecates ThumbnailImage and no longer populates it.
        if (cover.isNull())
            cover = asImage(meta.value(QMediaMetaData::ThumbnailImage));
#endif
#else
        const QImage cover = asImage(player->metaData(QMediaMetaData::CoverArtImage));
#endif
        artwork->setCover(cover);
        updateAudioArtworkVisibility();
    }

    void showError(const QString &message)
    {
        inputFailed = true;
        buffering->hide();
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
    const bool                   compactInlineAudio_;
    QPointer<QIODevice>          stream;
    QMediaPlayer                *player = nullptr;
    ITEVideoFrameWidget         *video;
    AudioArtworkWidget          *artwork = nullptr;
    QVBoxLayout                 *layout;
    VideoControls               *controls;
    QToolButton                 *windowButton;
    QToolButton                 *fullscreenButton;
    QPointer<VideoWindow>        floating;
    QPointer<QWidget>            inlineViewport;
    QPointer<ITEVideoController> downloadController;
    std::function<void()>        onDocked;
    bool                         movingToWindow               = false;
    bool                         hostedExternally_             = false;
    bool                         returnInlineOnFullscreenExit = false;
    QToolButton                 *button;
    ITEMediaSlider              *slider;
    QToolButton                 *downloadButton = nullptr;
    QPointer<ITEMediaState>      mediaState;
    QLabel                      *time;
    QWidget                     *transferIndicator = nullptr;
    QLabel                      *buffering;
    std::function<void()>        externalFullscreen;
    qint64                       initialPosition = 0;
    bool                         inputFailed     = false;

    void attachInput(QIODevice *input, const QUrl &resolved)
    {
        stream = input;
        source = resolved;
        static_cast<ITEMediaPlayer *>(player)->setInput(opener, input);
        iteConfigureMediaNetworkTimeout(player, source);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (input)
            player->setSourceDevice(input, source);
        else
            player->setSource(source);
#else
        player->setMedia(source, input);
#endif
    }
};
}

QToolButton *ITEVideoController::addPlayerAction(QWidget *player, const QString &id, QAction *action,
                                                 const QString &afterId, ControlVisibility visibility)
{
    auto *controls = player ? player->findChild<QWidget *>(QStringLiteral("qite-video-controls"),
                                                           Qt::FindDirectChildrenOnly) : nullptr;
    return controls ? static_cast<VideoControls *>(controls)->addPlayerAction(id, action, afterId, visibility)
                    : nullptr;
}

bool ITEVideoController::setPlayerActionVisibility(QWidget *player, const QString &id,
                                                    ControlVisibility visibility)
{
    auto *controls = player ? player->findChild<QWidget *>(QStringLiteral("qite-video-controls"),
                                                           Qt::FindDirectChildrenOnly) : nullptr;
    return controls && static_cast<VideoControls *>(controls)->setVisibility(id, visibility);
}

QIcon ITEVideoController::fullscreenIcon(const QPalette &palette, bool inward)
{
    const QIcon themed = QIcon::fromTheme(inward ? QStringLiteral("view-restore")
                                               : QStringLiteral("view-fullscreen"));
    if (!themed.isNull())
        return themed;
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette.color(QPalette::ButtonText), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (inward) {
        painter.drawLine(QPointF(4, 4), QPointF(10, 10));
        painter.drawLine(QPointF(10, 10), QPointF(10, 5));
        painter.drawLine(QPointF(10, 10), QPointF(5, 10));
        painter.drawLine(QPointF(20, 20), QPointF(14, 14));
        painter.drawLine(QPointF(14, 14), QPointF(14, 19));
        painter.drawLine(QPointF(14, 14), QPointF(19, 14));
    } else {
        painter.drawLine(QPointF(10, 10), QPointF(4, 4));
        painter.drawLine(QPointF(4, 4), QPointF(4, 9));
        painter.drawLine(QPointF(4, 4), QPointF(9, 4));
        painter.drawLine(QPointF(14, 14), QPointF(20, 20));
        painter.drawLine(QPointF(20, 20), QPointF(15, 20));
        painter.drawLine(QPointF(20, 20), QPointF(20, 15));
    }
    return QIcon(pixmap);
}

QWidget *ITEVideoController::createPlayer(const QUrl &url, ITEMediaOpener *opener, QWidget *parent,
                                          std::function<void()> fullscreen, qint64 position,
                                          bool audioPresentation, bool compactInlineAudio)
{
    QTextCharFormat format;
    format.setProperty(Source, url);
    if (opener)
        format.setProperty(Opener, QVariant::fromValue(QPointer<ITEMediaOpener>(opener)));
    auto widget = new VideoPlayer(format, parent, nullptr, {}, audioPresentation, compactInlineAudio);
    widget->setHostFullscreen(std::move(fullscreen));
    widget->setInitialPosition(position);
    QTimer::singleShot(0, widget, [widget] { widget->startPlayback(); });
    return widget;
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
