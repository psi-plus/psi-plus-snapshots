/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef PSIMEDIA_GSTVIDEOWIDGET_H
#define PSIMEDIA_GSTVIDEOWIDGET_H

#include "psimediaprovider.h"

#include <QImage>

namespace PsiMedia {

//----------------------------------------------------------------------------
// GstVideoWidget
//----------------------------------------------------------------------------
class GstVideoWidget : public QObject {
    Q_OBJECT

public:
    VideoWidgetContext *context;
    QImage              curImage;

    explicit GstVideoWidget(VideoWidgetContext *_context, QObject *parent = nullptr);

    void show_frame(const QImage &image);

private Q_SLOTS:
    void context_resized(const QSize &newSize);
    void context_paintEvent(QPainter *p);
};

} // namespace PsiMedia

#endif // PSIMEDIA_GSTVIDEOWIDGET_H
