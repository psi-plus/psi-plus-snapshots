/* SPDX-License-Identifier: Apache-2.0 */
#include "qitemedia.h"
#include "qitemedia_p.h"
#include <QPainter>
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
    p->setClipRect(rect);
    auto fill = [&](qreal start, qreal end, const QColor &color, bool stripe) {
        if (upsideDown) {
            const auto oldStart = start;
            start               = 1 - end;
            end                 = 1 - oldStart;
        }
        QRectF area(rect.left() + rect.width() * start, rect.top(), rect.width() * (end - start), rect.height());
        if (stripe) {
            const qreal height = qMax(qreal(1), rect.height() / 3);
            area.setTop(rect.center().y() - height / 2);
            area.setHeight(height);
        }
        p->fillRect(area, color);
    };
    if (state) {
        for (const auto &range : state->cachedRanges())
            fill(range.first, range.second, QColor(75, 145, 245, 155), false);
        if (state->downloading() && state->downloadProgress() > 0)
            fill(0, state->downloadProgress(), QColor(75, 145, 245, 155), false);
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
    setToolTip(tr("Blue: downloaded file data; marker: playback position"));
    update();
}

void ITEMediaSlider::paintEvent(QPaintEvent *)
{
    QStyleOptionSlider option;
    initStyleOption(&option);
    QPainter p(this);
    option.subControls = QStyle::SC_SliderGroove;
    style()->drawComplexControl(QStyle::CC_Slider, &option, &p, this);
    QRect       groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    // Match the handle's travel rather than the outer widget bounds.
    QRectF      track(handle.width() / 2., groove.center().y() - 3., width() - handle.width(), 6.);
    const qreal played = maximum() > minimum() ? qreal(value() - minimum()) / (maximum() - minimum()) : 0;
    itePaintMediaProgress(&p, track, state_, played, palette().color(QPalette::Highlight), option.upsideDown);
    option.subControls = QStyle::SC_SliderHandle | QStyle::SC_SliderTickmarks;
    style()->drawComplexControl(QStyle::CC_Slider, &option, &p, this);
}
