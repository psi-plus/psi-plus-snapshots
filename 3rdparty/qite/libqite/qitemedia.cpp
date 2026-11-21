/* SPDX-License-Identifier: Apache-2.0 */
#include "qitemedia.h"
#include "qitemedia_p.h"
#include <QMouseEvent>
#include <QHelpEvent>
#include <QHideEvent>
#include <QLocale>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>
#include <limits>

void ITEMediaState::setTransfer(quint64 available, quint64 total, TransferStatus status, const QString &error)
{
    available = total ? qMin(available, total) : 0;
    if (availableBytes_ == available && totalBytes_ == total && transferStatus_ == status && transferError_ == error)
        return;
    availableBytes_ = available;
    totalBytes_ = total;
    transferStatus_ = status;
    transferError_ = error;
    emit changed();
}

namespace {
QString mediaByteSize(quint64 bytes)
{
    // The state counters are unsigned; avoid narrowing above qint64's range.
    return QLocale().formattedDataSize(qint64(qMin(bytes, quint64(std::numeric_limits<qint64>::max()))),
                                       1, QLocale::DataSizeIecFormat);
}

class MediaTransferIndicator final : public QWidget {
public:
    MediaTransferIndicator(ITEMediaState *state, QWidget *parent) : QWidget(parent), state_(state)
    {
        setObjectName(QStringLiteral("qite-video-transfer-indicator"));
        setFixedSize(24, 24);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        if (state_) {
            connect(state_, &ITEMediaState::changed, this, [this] { refresh(); });
            connect(state_, &QObject::destroyed, this, [this] {
                state_ = nullptr;
                refresh();
            });
        }
        animation_.setInterval(120);
        connect(&animation_, &QTimer::timeout, this, [this] {
            phase_ = (phase_ + 16) % 360;
            update();
        });
        refresh();
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip && state_) {
            const auto *help = static_cast<QHelpEvent *>(event);
            tooltipRequested_ = true;
            tooltipPosition_ = help->globalPos();
            QToolTip::showText(tooltipPosition_, toolTip(), this, rect());
            event->accept();
            return true;
        }
        if (event->type() == QEvent::Leave)
            tooltipRequested_ = false;
        return QWidget::event(event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        syncAnimation();
    }

    void hideEvent(QHideEvent *event) override
    {
        animation_.stop();
        tooltipRequested_ = false;
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        if (!state_)
            return;
        const qreal fraction = state_->totalBytes()
            ? qreal(state_->availableBytes()) / qreal(state_->totalBytes()) : 0.0;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal scale = qMin(width(), height()) / 24.0;
        painter.translate(width() / 2.0, height() / 2.0);
        painter.scale(scale, scale);
        QPen ring(QColor(125, 132, 144, 200), 2.4, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(-8, -8, 16, 16));
        const auto status = state_->transferStatus();
        QColor ink = status == ITEMediaState::TransferStatus::Failed ? QColor(243, 149, 149)
            : status == ITEMediaState::TransferStatus::Waiting
                || status == ITEMediaState::TransferStatus::Paused ? QColor(205, 211, 221)
            : QColor(115, 184, 248);
        painter.setPen(QPen(ink, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal sweep = status == ITEMediaState::TransferStatus::Complete
            ? 360.0 : qBound(qreal(0), fraction * 360, qreal(360));
        if (sweep > 0)
            painter.drawArc(QRectF(-8, -8, 16, 16), (90 - (status == ITEMediaState::TransferStatus::Transferring ? phase_ : 0)) * 16,
                            -qRound(sweep * 16));
        else if (status == ITEMediaState::TransferStatus::Transferring)
            painter.drawArc(QRectF(-8, -8, 16, 16), (90 - phase_) * 16, -55 * 16);
        painter.setPen(QPen(QColor(225, 232, 243), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        switch (status) {
        case ITEMediaState::TransferStatus::Transferring:
            painter.setBrush(QColor(225, 232, 243));
            painter.drawEllipse(QPointF(0, 0), 1.5, 1.5);
            break;
        case ITEMediaState::TransferStatus::Failed:
            painter.drawLine(QPointF(-2.5, -2.5), QPointF(2.5, 2.5));
            painter.drawLine(QPointF(2.5, -2.5), QPointF(-2.5, 2.5));
            break;
        case ITEMediaState::TransferStatus::Complete:
            painter.drawLine(QPointF(-3.5, 0), QPointF(-1, 2.5));
            painter.drawLine(QPointF(-1, 2.5), QPointF(4, -3));
            break;
        case ITEMediaState::TransferStatus::Waiting:
        case ITEMediaState::TransferStatus::Paused:
            painter.drawLine(QPointF(-2.5, -3), QPointF(-2.5, 3));
            painter.drawLine(QPointF(2.5, -3), QPointF(2.5, 3));
            break;
        }
    }

private:
    void syncAnimation()
    {
        const bool animate = isVisible() && state_ && state_->totalBytes()
            && state_->transferStatus() == ITEMediaState::TransferStatus::Transferring;
        if (animate && !animation_.isActive())
            animation_.start();
        else if (!animate && animation_.isActive()) {
            animation_.stop();
            phase_ = 0;
        }
    }

    void refresh()
    {
        const bool available = state_ && state_->totalBytes();
        setVisible(available);
        if (!available) {
            animation_.stop();
            setToolTip({});
            return;
        }
        QString status;
        switch (state_->transferStatus()) {
        case ITEMediaState::TransferStatus::Waiting:
            status = tr("Waiting for media");
            break;
        case ITEMediaState::TransferStatus::Transferring:
            status = tr("Downloading media");
            break;
        case ITEMediaState::TransferStatus::Paused:
            status = tr("Media download paused");
            break;
        case ITEMediaState::TransferStatus::Failed:
            status = tr("Media download failed");
            break;
        case ITEMediaState::TransferStatus::Complete:
            status = tr("Available offline");
            break;
        }
        const auto total = state_->totalBytes();
        const auto cached = state_->availableBytes();
        QString tooltip;
        if (state_->transferStatus() == ITEMediaState::TransferStatus::Complete) {
            // Completed media needs one size, not a redundant 100% byte ratio.
            tooltip = tr("Available offline (%1)").arg(mediaByteSize(total));
        } else {
            tooltip = status + QStringLiteral("\n") + tr("Cached: %1 / %2 (%3%)")
                .arg(mediaByteSize(cached), mediaByteSize(total))
                .arg(qRound(100.0 * qreal(cached) / qreal(total)));
            if (state_->transferStatus() == ITEMediaState::TransferStatus::Failed && !state_->transferError().isEmpty())
                tooltip += QStringLiteral("\n") + state_->transferError();
        }
        setToolTip(tooltip);
        setAccessibleName(tooltip);
        syncAnimation();
        update();
        // The native tooltip would otherwise retain the initial byte count
        // until the pointer leaves and re-enters. Refresh only on changes.
        if (tooltipRequested_ && underMouse() && QToolTip::isVisible())
            QToolTip::showText(tooltipPosition_, tooltip, this, rect());
    }

    QPointer<ITEMediaState> state_;
    QTimer                 animation_;
    int                    phase_ = 0;
    bool                   tooltipRequested_ = false;
    QPoint                 tooltipPosition_;
};
} // namespace

QWidget *iteCreateMediaTransferIndicator(ITEMediaState *state, QWidget *parent)
{
    return new MediaTransferIndicator(state, parent);
}

void ITEMediaState::setCachedRanges(Ranges ranges)
{
    Ranges normalized;
    for (auto range : ranges) {
        if (!std::isfinite(range.first) || !std::isfinite(range.second))
            continue;
        range.first  = qBound(qreal(0), range.first, qreal(1));
        range.second = qBound(qreal(0), range.second, qreal(1));
        if (range.first < range.second)
            normalized.append(range);
    }
    std::sort(normalized.begin(), normalized.end());
    ranges.clear();
    for (const auto &range : normalized) {
        if (!ranges.isEmpty() && range.first <= ranges.last().second)
            ranges.last().second = qMax(ranges.last().second, range.second);
        else
            ranges.append(range);
    }
    if (ranges_ == ranges)
        return;
    ranges_ = std::move(ranges);
    emit changed();
}

void ITEMediaState::setDownload(bool active, qreal progress, const QString &error)
{
    progress = std::isfinite(progress) && progress >= 0 ? qMin(progress, qreal(1)) : -1;
    if (downloading_ == active && progress_ == progress && error_ == error)
        return;
    downloading_ = active;
    progress_    = progress;
    error_       = error;
    emit changed();
}

QString iteMediaDownloadText(const ITEMediaState *state)
{
    if (state && !state->sourceError().isEmpty())
        return ITEMediaState::tr("Source error: %1. Click to retry.").arg(state->sourceError());
    if (state && !state->errorString().isEmpty())
        return ITEMediaState::tr("Download failed: %1. Click to retry.").arg(state->errorString());
    if (state && state->downloading()) {
        if (state->downloadProgress() >= 0)
            return ITEMediaState::tr("Downloading: %1%").arg(qRound(state->downloadProgress() * 100));
        return ITEMediaState::tr("Downloading…");
    }
    return ITEMediaState::tr("Download");
}

void itePaintMediaProgress(QPainter *p, const QRectF &rect, const ITEMediaState *state, qreal played,
                           const QColor &color, bool upsideDown)
{
    p->save();
    p->setClipRect(rect, Qt::IntersectClip);
    auto fill = [&](qreal start, qreal end, const QColor &color, bool stripe) {
        if (upsideDown) {
            const auto oldStart = start;
            start               = 1 - end;
            end                 = 1 - oldStart;
        }
        QRectF area(rect.left() + rect.width() * start, rect.top(), rect.width() * (end - start), rect.height());
        if (stripe) {
            const qreal height = qMin(rect.height(), qMax(qreal(2), rect.height() / 2));
            area.setTop(rect.center().y() - height / 2);
            area.setHeight(height);
        }
        p->fillRect(area, color);
    };
    if (state) {
        // Cached coverage is a map of verified FILE BYTES, not elapsed
        // playback time. Neutral gray makes this distinct from the
        // highlight-colored playback position. A save/download progress
        // fraction is not evidence that those bytes are cached.
        for (const auto &range : state->cachedRanges())
            fill(range.first, range.second, QColor(155, 158, 165, 220), false);
    }
    fill(0, qBound(qreal(0), played, qreal(1)), color, true);
    p->restore();
}

void ITEMediaSlider::setMediaState(ITEMediaState *state)
{
    QObject::disconnect(connection_);
    QObject::disconnect(destroyed_);
    state_ = state;
    if (state) {
        connection_ = connect(state, &ITEMediaState::changed, this, [this] { update(); });
        destroyed_  = connect(state, &QObject::destroyed, this, [this] { update(); });
    }
    setToolTip(tr("Gray: cached file bytes (not playback time); highlighted line: playback position"));
    update();
}

void ITEMediaSlider::mousePressEvent(QMouseEvent *event)
{
    grooveSeekPressed_ = false;
    if (event->button() == Qt::LeftButton && isEnabled()) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

        // QSlider's default groove click performs a page step, which is a
        // particularly small seek for long media. Map the click to the
        // handle's actual travel range instead, retaining normal handle drag.
        if (groove.contains(event->pos()) && !handle.contains(event->pos())) {
            const int start = orientation() == Qt::Horizontal ? groove.left() + handle.width() / 2
                                                               : groove.top() + handle.height() / 2;
            const int span = orientation() == Qt::Horizontal ? groove.width() - handle.width()
                                                              : groove.height() - handle.height();
            const int pixel = (orientation() == Qt::Horizontal ? event->pos().x() : event->pos().y()) - start;
            setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), pixel, span, option.upsideDown));
            grooveSeekPressed_ = true;
            event->accept();
            return;
        }
    }
    QSlider::mousePressEvent(event);
}

void ITEMediaSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (grooveSeekPressed_ && event->button() == Qt::LeftButton) {
        // A direct groove click changes the value on press, without entering
        // QSlider's pressed state. Its base release handler would ignore this
        // event and Qt could propagate it to the player as a click-to-pause.
        grooveSeekPressed_ = false;
        event->accept();
        return;
    }
    QSlider::mouseReleaseEvent(event);
}

void ITEMediaSlider::paintEvent(QPaintEvent *)
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    QPainter p(this);
    const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);

    // Draw a single, self-contained groove. Painting the native groove first
    // lets some styles draw their own "played" segment with different origins
    // and height, causing an apparent offset and a stray one-pixel edge.
    auto handleCenter = [this, &option](int position) {
        QStyleOptionSlider edge = option;
        edge.sliderPosition = position;
        edge.sliderValue    = position;
        return style()->subControlRect(QStyle::CC_Slider, &edge, QStyle::SC_SliderHandle, this).center().x();
    };
    const qreal first = handleCenter(minimum());
    const qreal last  = handleCenter(maximum());
    const QRectF track(qMin(first, last), groove.top(), qAbs(last - first), groove.height());
    if (track.width() > 0 && track.height() > 0) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        const qreal radius = track.height() / 2;
        clip.addRoundedRect(track, radius, radius);
        p.setClipPath(clip);
        p.fillRect(track, palette().color(QPalette::Mid));
        const qreal played = maximum() > minimum() ? qreal(value() - minimum()) / (maximum() - minimum()) : 0;
        itePaintMediaProgress(&p, track, state_, played, palette().color(QPalette::Highlight), option.upsideDown);
        p.restore();
    }
    option.subControls = QStyle::SC_SliderHandle | QStyle::SC_SliderTickmarks;
    style()->drawComplexControl(QStyle::CC_Slider, &option, &p, this);
}
