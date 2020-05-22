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

#include "gstvideowidget.h"

#include <QPainter>
#include <QPalette>
#include <QWidget>

namespace PsiMedia {

GstVideoWidget::GstVideoWidget(VideoWidgetContext *_context, QObject *parent) : QObject(parent), context(_context)
{
    QPalette palette;
    palette.setColor(context->qwidget()->backgroundRole(), Qt::black);
    context->qwidget()->setPalette(palette);
    context->qwidget()->setAutoFillBackground(true);

    connect(context->qobject(), SIGNAL(resized(const QSize &)), SLOT(context_resized(const QSize &)));
    connect(context->qobject(), SIGNAL(paintEvent(QPainter *)), SLOT(context_paintEvent(QPainter *)));
}

void GstVideoWidget::show_frame(const QImage &image)
{
    curImage = image;
    context->qwidget()->update();
}

void GstVideoWidget::context_resized(const QSize &newSize) { Q_UNUSED(newSize); }

void GstVideoWidget::context_paintEvent(QPainter *p)
{
    if (curImage.isNull())
        return;

    QSize size    = context->qwidget()->size();
    QSize newSize = curImage.size();
    newSize.scale(size, Qt::KeepAspectRatio);
    int xoff = 0;
    int yoff = 0;
    if (newSize.width() < size.width())
        xoff = (size.width() - newSize.width()) / 2;
    else if (newSize.height() < size.height())
        yoff = (size.height() - newSize.height()) / 2;

    // ideally, the backend will follow desired_size() and give
    //   us images that generally don't need resizing
    QImage i;
    if (curImage.size() != newSize) {
        // the IgnoreAspectRatio is okay here, since we
        //   used KeepAspectRatio earlier
        i = curImage.scaled(newSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else
        i = curImage;

    p->drawImage(xoff, yoff, i);
}

} // namespace PsiMedia
