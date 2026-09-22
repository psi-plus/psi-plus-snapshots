/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "gstvideowidget.h"

#include <QApplication>
#include <QDebug>
#include <QPainter>
#include <QWidget>

class TestVideoContext final : public QObject, public PsiMedia::VideoWidgetContext {
    Q_OBJECT

public:
    QObject *qobject() override { return this; }
    QWidget *qwidget() override { return &widget; }

    void setVideoSize(const QSize &size) override
    {
        lastSize = size;
        ++sizeUpdates;
    }

    QWidget widget;
    QSize   lastSize;
    int     sizeUpdates = 0;

signals:
    void resized(const QSize &newSize);
    void paintEvent(QPainter *p);
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    TestVideoContext         context;
    PsiMedia::GstVideoWidget output(&context);

    output.show_frame(QImage(320, 240, QImage::Format_RGB32));
    if (context.lastSize != QSize(320, 240) || context.sizeUpdates != 1) {
        qCritical() << "first decoded frame did not publish its video size" << context.lastSize << context.sizeUpdates;
        return 1;
    }

    output.show_frame(QImage(320, 240, QImage::Format_RGB32));
    if (context.sizeUpdates != 1) {
        qCritical() << "same-size frame emitted a redundant size update" << context.sizeUpdates;
        return 2;
    }

    output.show_frame(QImage(640, 360, QImage::Format_RGB32));
    if (context.lastSize != QSize(640, 360) || context.sizeUpdates != 2) {
        qCritical() << "resolution change did not publish the new video size" << context.lastSize
                    << context.sizeUpdates;
        return 3;
    }

    {
        auto                    *dyingContext = new TestVideoContext;
        PsiMedia::GstVideoWidget survivingOutput(dyingContext);
        survivingOutput.show_frame(QImage(160, 120, QImage::Format_RGB32));
        delete dyingContext;

        // Backend teardown can outlive the UI VideoWidgetContext. Clearing the
        // last frame must be a no-op instead of dereferencing the dead context.
        survivingOutput.show_frame(QImage());
    }

    qInfo() << "decoded video size/lifetime regression passed";
    return 0;
}

#include "gstvideowidget_size.moc"
