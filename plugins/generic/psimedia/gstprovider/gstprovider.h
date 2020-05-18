/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 * Copyright (C) 2020  Sergey Ilinykh
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

#include "psimediaprovider.h"

#include <QPointer>
#include <QThread>
#include <QVariantMap>

namespace PsiMedia {

class GstMainLoop;

class GstProvider : public QObject, public Provider {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::Provider)

public:
    QThread               gstEventLoopThread;
    QPointer<GstMainLoop> gstEventLoop;

    GstProvider(const QVariantMap &params = QVariantMap());
    ~GstProvider() override;
    QObject *          qobject() override;
    bool               init() override;
    bool               isInitialized() const override;
    static QString     creditName();
    QString            creditText();
    FeaturesContext *  createFeatures() override;
    RtpSessionContext *createRtpSession() override;

signals:
    void initialized();
};

}
