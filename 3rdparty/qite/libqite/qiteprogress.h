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

#ifndef QITEPROGRESS_H
#define QITEPROGRESS_H

#include <QCursor>
#include <QObject>

#include "qite.h"

class ITEProgressController : public InteractiveTextElementController {
    Q_OBJECT

    QCursor _cursor;

    // geometry
    QSize   elementSize;
    QRect   bgRect;
    QRect   metaRect;
    int     bgOutlineWidth;
    double  baseSize;
    double  bgRectRadius;
    QPointF btnCenter;
    int     btnRadius;
    int     signSize;
    int     scaleOutlineWidth;
    QRectF  scaleRect, scaleFillRect;
    int     lastFontSize = 0;

    bool isOnButton(const QPoint &pos, const QRect &rect);
    void updateGeomtry();

public:
    ITEProgressController(InteractiveText *itc);

    QSizeF intrinsicSize(QTextDocument *doc, int posInDocument, const QTextFormat &format);
    void   drawITE(QPainter *painter, const QRectF &rect, int posInDocument, const QTextFormat &format);

    QTextCharFormat makeFormat() const;
    void            insert(double min, double max, const QString &text);
    QCursor         cursor(); // cursor form after last mose events

protected:
    bool mouseEvent(const InteractiveTextElementController::Event &event, const QRect &rect, QTextCursor &selected);
    void hideEvent(QTextCursor &selected);
};

#endif // QITEPROGRESS_H
