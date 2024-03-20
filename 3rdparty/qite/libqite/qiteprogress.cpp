/*
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
*/

#include "qiteprogress.h"

#include <QEvent>
#include <QHoverEvent>
#include <QPainter>
#include <QTextEdit>
#include <QTimer>
#include <QVector2D>
#include <QtGlobal>

class ProgressMessageFormat : public InteractiveTextFormat {
public:
    enum Property {
        Text,
        MinValue,
        MaxValue,
        CurrentValue, /* in pixels */
        State,
    };

    enum Flag { Playing = 0x1, MouseOnButton = 0x2, MouseOnTrackbar = 0x4 };
    Q_DECLARE_FLAGS(Flags, Flag)

    using InteractiveTextFormat::InteractiveTextFormat;

    static ProgressMessageFormat fromCharFormat(const QTextCharFormat &fmt)
    {
        return static_cast<ProgressMessageFormat>(fmt);
    }

    QString text() const { return property(ProgressMessageFormat::Text).toString(); }

    Flags state() const { return Flags(property(ProgressMessageFormat::State).toInt()); }
    void  setState(const Flags &state) { setProperty(State, int(state)); }

    double currentValue() const { return property(ProgressMessageFormat::CurrentValue).toDouble(); }
    void   setCurrentValue(double position) { setProperty(ProgressMessageFormat::CurrentValue, position); }

    double minValue() const { return property(ProgressMessageFormat::MinValue).toDouble(); }
    void   setMinValue(double position) { setProperty(ProgressMessageFormat::MinValue, position); }

    double maxValue() const { return property(ProgressMessageFormat::MaxValue).toDouble(); }
    void   setMaxValue(double position) { setProperty(ProgressMessageFormat::MaxValue, position); }
};
Q_DECLARE_OPERATORS_FOR_FLAGS(ProgressMessageFormat::Flags)

//----------------------------------------------------------------------------
// ITEProgressController
//----------------------------------------------------------------------------
QSizeF ITEProgressController::intrinsicSize(QTextDocument *doc, int posInDocument, const QTextFormat &format)
{
    Q_UNUSED(doc)
    Q_UNUSED(posInDocument)
    const QTextCharFormat charFormat = format.toCharFormat();
    auto                  psize      = QFontMetrics(charFormat.font()).height();
    if (lastFontSize != psize) {
        lastFontSize = psize;
        updateGeomtry();
    }
    return elementSize;
}

void ITEProgressController::updateGeomtry()
{
    // compute geomtry of player
    baseSize           = lastFontSize / 12.0;
    int elementPadding = int(baseSize * 4);

    bgOutlineWidth = baseSize < 2 ? 2 : int(baseSize);

    btnRadius         = int(baseSize * 10);
    int elementHeight = btnRadius * 2 + int(elementPadding * 2);

    auto rightPadding = int(baseSize * 5);
    // elementHeight already includes 2 paddings: to the lest and to the right of button
    elementSize = QSize(elementHeight + int(100 * baseSize) + rightPadding, elementHeight);

    bgRect = QRect(QPoint(0, 0), elementSize);
    bgRect.adjust(bgOutlineWidth / 2, bgOutlineWidth / 2, -bgOutlineWidth / 2,
                  -bgOutlineWidth / 2); // outline should fit the format rect.
    bgRectRadius = bgRect.height() / 5;

    btnCenter = QPoint(elementSize.height() / 2, elementSize.height() / 2);

    signSize = btnRadius / 2;

    // next to the button we need histgram/title and scale.
    int left  = elementHeight;
    int right = elementSize.width() - rightPadding;

    metaRect = QRect(QPoint(left, bgRect.top() + int(baseSize * 3)),
                     QPoint(right, bgRect.top() + int(bgRect.height() * 0.5)));

    // draw scale
    scaleOutlineWidth = bgOutlineWidth;
    QPointF scaleTopLeft(
        left, metaRect.bottom() + baseSize * 4); // = bgRect.topLeft() + QPointF(left, bgRect.height() * 0.7);
    QPointF scaleBottomRight(right, scaleTopLeft.y() + baseSize * 4);
    scaleRect     = QRectF(scaleTopLeft, scaleBottomRight);
    scaleFillRect = scaleRect.adjusted(scaleOutlineWidth / 2, scaleOutlineWidth / 2, -scaleOutlineWidth / 2,
                                       -scaleOutlineWidth / 2);
}

void ITEProgressController::drawITE(QPainter *painter, const QRectF &rect, [[maybe_unused]] int posInDocument,
                                    const QTextFormat &format)
{
    const ProgressMessageFormat audioFormat = ProgressMessageFormat::fromCharFormat(format.toCharFormat());
    // qDebug() << audioFormat.id();

    painter->setRenderHints(QPainter::Antialiasing);

    QPen bgPen(QColor(100, 200, 100)); // TODO name all the magic colors
    bgPen.setWidth(bgOutlineWidth);
    painter->setPen(bgPen);
    painter->setBrush(QColor(150, 250, 150));
    painter->drawRoundedRect(bgRect.translated(int(rect.left()), int(rect.top())), bgRectRadius, bgRectRadius);

    // draw button
    if (audioFormat.state() & ProgressMessageFormat::MouseOnButton) {
        painter->setBrush(QColor(130, 230, 130));
    } else {
        painter->setBrush(QColor(120, 220, 120));
    }
    auto xBtnCenter = btnCenter + rect.topLeft();
    painter->drawEllipse(xBtnCenter, btnRadius, btnRadius);

    // draw pause/play
    QPen signPen((QColor(Qt::white)));
    signPen.setWidth(bgOutlineWidth);
    painter->setPen(signPen);
    painter->setBrush(QColor(Qt::white));
    bool isPlaying = audioFormat.state() & ProgressMessageFormat::Playing;
    if (isPlaying) {
        QRectF bar(0, 0, signSize / 3, signSize * 2);
        bar.moveCenter(xBtnCenter - QPointF(signSize / 2, 0));
        painter->drawRect(bar);
        bar.moveCenter(xBtnCenter + QPointF(signSize / 2, 0));
        painter->drawRect(bar);
    } else {
        QPointF play[3] = { xBtnCenter - QPoint(signSize / 2, signSize), xBtnCenter - QPoint(signSize / 2, -signSize),
                            xBtnCenter + QPoint(signSize, 0) };
        painter->drawConvexPolygon(play, 3);
    }

    // draw scale
    QPen scalePen(QColor(100, 200, 100));
    scalePen.setWidth(scaleOutlineWidth);
    painter->setPen(scalePen);
    painter->setBrush(QColor(120, 220, 120));
    QRectF xScaleRect(scaleRect.translated(rect.topLeft()));
    painter->drawRoundedRect(xScaleRect, scaleRect.height() / 2, scaleRect.height() / 2);

    // draw played part
    auto playPos = audioFormat.currentValue();
    if (playPos) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(170, 255, 170));
        QRectF playedRect(scaleFillRect.translated(rect.topLeft())); // to the width of the scale border
        playedRect.setWidth(playPos);
        painter->drawRoundedRect(playedRect, playedRect.height() / 2, playedRect.height() / 2);
    }

    painter->setPen(QColor(70, 150, 70));
    painter->drawText(metaRect.translated(rect.topLeft().toPoint()), audioFormat.text());
}

QTextCharFormat ITEProgressController::makeFormat() const
{
    ProgressMessageFormat fmt(objectType, itc->nextId());
    fmt.setFontPointSize(itc->textEdit()->currentFont().pointSize());
    return fmt;
}

void ITEProgressController::insert([[maybe_unused]] double min, [[maybe_unused]] double max,
                                   [[maybe_unused]] const QString &text)
{
    auto fmt = makeFormat();
    itc->insert(static_cast<InteractiveTextFormat>(fmt));
}

bool ITEProgressController::mouseEvent(const Event &event, const QRect &rect, QTextCursor &selected)
{
    Q_UNUSED(rect);
    bool onButton   = false;
    bool onTrackbar = false;
    if (event.type != EventType::Leave) {
        onButton = isOnButton(event.pos, bgRect);
        if (!onButton) {
            onTrackbar = scaleRect.contains(event.pos);
        }
    }
    if (onButton || onTrackbar) {
        _cursor = QCursor(Qt::PointingHandCursor);
    } else {
        _cursor = QCursor(Qt::ArrowCursor);
    }

    ProgressMessageFormat        format            = ProgressMessageFormat::fromCharFormat(selected.charFormat());
    ProgressMessageFormat::Flags state             = format.state();
    bool                         onButtonChanged   = bool(state & ProgressMessageFormat::MouseOnButton) != onButton;
    bool                         onTrackbarChanged = bool(state & ProgressMessageFormat::MouseOnTrackbar) != onTrackbar;
    // bool                         playStateChanged  = false;
    // bool                         positionSet       = false;

    if (onButtonChanged) {
        state ^= ProgressMessageFormat::MouseOnButton;
    }
    if (onTrackbarChanged) {
        state ^= ProgressMessageFormat::MouseOnTrackbar;
    }
#if 0
    auto playerId = format.id();
    if (event.type == EventType::Click) {
        if (onButton) {
            playStateChanged = true;
            state ^= ProgressMessageFormat::Playing;
            auto player = activePlayers.value(playerId);
            if (state & ProgressMessageFormat::Playing) {
                if (!player) {
                    player = new QMediaPlayer(this);
                    player->setProperty("playerId", playerId);
                    player->setProperty("cursorPos", selected.anchor());
                    activePlayers.insert(playerId, player);
                    ITEMediaOpener *opener = format.mediaOpener();
                    QUrl            url    = format.url();
                    QIODevice *     stream = opener ? opener->open(url) : nullptr;
                    if (stream)
                        connect(player, &QMediaPlayer::destroyed, this, [opener, stream]() { opener->close(stream); });
                    player->setMedia(url, stream);
                    auto part = double(format.playPosition()) / double(scaleFillRect.width());

                    if (player->duration() > 0) {
                        player->setPosition(qint64(player->duration() * part));
                        player->setNotifyInterval(int(player->duration() / double(metaRect.width()) * 3.0)); // 3 px
                        connect(player, SIGNAL(positionChanged(qint64)), this, SLOT(playerPositionChanged(qint64)));
                    } else {
                        connect(player, &QMediaPlayer::durationChanged, [player, part, this](qint64 duration) {
                            // the timer is a workaround for some Qt bug
                            QTimer::singleShot(0, [player, part, this, duration]() {
                                if (part > 0) { // don't jump back if event came quite late
                                    player->setPosition(qint64(duration * part));
                                }
                                // qDebug() << int(duration / double(metaRect.width()));
                                player->setNotifyInterval(int(duration / 1000.0 / double(metaRect.width()) * 3.0));
                                connect(player, SIGNAL(positionChanged(qint64)), this,
                                        SLOT(playerPositionChanged(qint64)));
                            });
                        });
                        player->setNotifyInterval(50); // while we don't know duration, lets use quite small value
                    }

                    // check for title in metadata
                    connect(player, &QMediaPlayer::metaDataAvailableChanged, this, [this, player](bool available) {
                        if (available) {
                            auto title = player->metaData(QMediaMetaData::Title).toString();
                            if (title.isEmpty()) {
                                return;
                            }
                            quint32     playerId      = player->property("playerId").toUInt();
                            int         textCursorPos = player->property("cursorPos").toInt();
                            QTextCursor cursor        = itc->findElement(playerId, textCursorPos);
                            if (cursor.isNull()) {
                                return;
                            }
                            auto format = ProgressMessageFormat::fromCharFormat(cursor.charFormat().toCharFormat());
                            if (format.metaData().type() == QVariant::List) {
                                return; // seems we have amplitudes already
                            }
                            format.setMetaData(title);
                            cursor.setCharFormat(format);
                        }
                    });

                    // try to extract from metadata and store amplitudes
                    connect(player,
                            static_cast<void (QMediaPlayer::*)(const QString &, const QVariant &)>(
                                &QMediaPlayer::metaDataChanged),
                            [=](const QString &key, const QVariant &value) {
                                QString comment;
                                int     index = 0;
                                if (key != QMediaMetaData::Comment || (comment = value.toString()).isEmpty()
                                    || !comment.startsWith(QLatin1String("AMPLDIAGSTART"))
                                    || (index = comment.indexOf("AMPLDIAGEND")) == -1) {
                                    return; // In comment we keep amplitudes. We don't expect anything else
                                }
                                auto sl
                                    = comment
                                          .mid(int(sizeof("AMPLDIAGSTART")), index - int(sizeof("AMPLDIAGSTART")) - 1)
                                          .split(",");
                                QList<float> amplitudes;
                                amplitudes.reserve(sl.size());
                                std::transform(sl.constBegin(), sl.constEnd(), std::back_inserter(amplitudes),
                                               [](const QString &v) {
                                                   auto fv = v.toFloat() / float(255.0);
                                                   if (fv > 1) {
                                                       return 1.0f;
                                                   }
                                                   return fv;
                                               });

                                quint32     playerId      = player->property("playerId").toUInt();
                                int         textCursorPos = player->property("cursorPos").toInt();
                                QTextCursor cursor        = itc->findElement(playerId, textCursorPos);
                                if (cursor.isNull()) {
                                    return;
                                }

                                auto format = ProgressMessageFormat::fromCharFormat(cursor.charFormat().toCharFormat());
                                format.setMetaData(QVariant::fromValue<decltype(amplitudes)>(amplitudes));
                                cursor.setCharFormat(format);
                            });

                    connect(player, SIGNAL(stateChanged(QMediaPlayer::State)), this,
                            SLOT(playerStateChanged(QMediaPlayer::State)));
                    QObject::connect(player, &QMediaPlayer::mediaStatusChanged,
                                     [=]() { qDebug() << "Media status changed:" << player->mediaStatus(); });
                    QObject::connect(player,
                                     static_cast<void (QMediaPlayer::*)(QMediaPlayer::Error)>(&QMediaPlayer::error),
                                     [=](QMediaPlayer::Error error) { qDebug() << "Error occurred:" << error; });
                }
                // player->setVolume(0);
                player->play();
            } else {
                if (player) {
                    player->pause();
                    // player->disconnect(this);activePlayers.take(playerId)->deleteLater();
                }
            }
        } else if (onTrackbar) {
            // include outline to clickable area but compute only for inner part
            double part;
            if (event.pos.x() < scaleFillRect.left()) {
                part = 0;
            } else if (event.pos.x() >= scaleFillRect.right()) {
                part = 1;
            } else {
                part = double(event.pos.x() - scaleFillRect.left()) / double(scaleFillRect.width());
            }
            auto player = activePlayers.value(playerId);
            if (player) {
                qDebug("Set position to %d", int(part * 100));
                player->setPosition(qint64(player->duration() * part));
            } // else it's not playing likely
            format.setCurrentValue(quint32(double(scaleFillRect.width()) * part));
            positionSet = true;
        }
    }

    if (onButtonChanged || playStateChanged || positionSet) {
        format.setState(state);
        selected.setCharFormat(format);
    }
#endif
    return true;
}

void ITEProgressController::hideEvent(QTextCursor &selected)
{
    auto fmt = ProgressMessageFormat::fromCharFormat(selected.charFormat());
#if 0
    auto player = activePlayers.value(fmt.id());
    // qDebug() << "hiding player" << fmt.id();
    if (player) {
        player->stop();
    }
#endif
}

bool ITEProgressController::isOnButton(const QPoint &pos, const QRect &rect)
{
    QPoint rel = pos - rect.topLeft();
    return QVector2D(btnCenter).distanceToPoint(QVector2D(rel)) <= btnRadius;
}

ITEProgressController::ITEProgressController(InteractiveText *itc) : InteractiveTextElementController(itc) { }

QCursor ITEProgressController::cursor() { return _cursor; }
