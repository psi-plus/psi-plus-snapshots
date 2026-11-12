/* SPDX-License-Identifier: Apache-2.0 */
#include "qitemedia.h"
#include "qitemedia_p.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>

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
