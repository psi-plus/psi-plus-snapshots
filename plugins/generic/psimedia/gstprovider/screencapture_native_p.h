// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "psimedia/psimediascreencapture.h"
#include "psimediaprovider.h"
#include <functional>

// Private GStreamer-provider data: never exposed through the screen capture ABI.
namespace PsiMedia::ScreenCaptureNative {
struct Source {
    PScreenCaptureSource::Kind kind;
    QString                    label;
    QString                    pipeline;
    QByteArray                 thumbnail;
};
using Callback = std::function<void(QList<Source>, QString)>;
bool available();
// Callback on the receiver's thread; suppressed after receiver destruction.
void sources(QObject *receiver, Callback callback);
}
