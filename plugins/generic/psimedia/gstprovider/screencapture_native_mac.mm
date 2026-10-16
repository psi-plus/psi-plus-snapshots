// SPDX-License-Identifier: GPL-2.0-or-later
#include "screencapture_native_p.h"
#import <CoreGraphics/CoreGraphics.h>
#include <QObject>
#include <QPointer>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

namespace PsiMedia::ScreenCaptureNative {
bool available()
{
    if (@available(macOS 12.3, *))
        return true;
    return false;
}
void sources(QObject *receiver, Callback callback)
{
    const QPointer<QObject> guard(receiver);
    if (@available(macOS 12.3, *)) {
        // Enumeration exposes other apps' windows, so request consent only here.
        const bool allowed = CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess();
        if (!guard)
            return;
        if (!allowed) {
            callback(
                {},
                QObject::tr("Allow Screen Recording for this application in System Settings, then try sharing again. "
                            "macOS may require restarting the application after changing this permission."));
            return;
        }
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:YES
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent *content, NSError *error) {
                                         QList<Source> result;
                                         QString       failure;
                                         if (error) {
                                             failure = QString::fromUtf8(error.localizedDescription.UTF8String);
                                         } else {
                                             for (SCDisplay *display in content.displays) {
                                                 result.append({ PScreenCaptureSource::Display,
                                                                 QObject::tr("Display %1 (%2 × %3)")
                                                                     .arg(display.displayID)
                                                                     .arg(display.width)
                                                                     .arg(display.height),
                                                                 QStringLiteral("psiscreencapturekitsrc display-id=%1")
                                                                     .arg(display.displayID) });
                                             }
                                             for (SCWindow *window in content.windows) {
                                                 if (window.frame.size.width <= 0 || window.frame.size.height <= 0
                                                     || !window.owningApplication)
                                                     continue;
                                                 const QString app = QString::fromUtf8(
                                                     window.owningApplication.applicationName.UTF8String);
                                                 const QString title = QString::fromUtf8(window.title.UTF8String);
                                                 result.append(
                                                     { PScreenCaptureSource::Window,
                                                       title.isEmpty() ? app : app + QStringLiteral(" — ") + title,
                                                       QStringLiteral("psiscreencapturekitsrc window-id=%1")
                                                           .arg(window.windowID) });
                                             }
                                         }
                                         dispatch_async(dispatch_get_main_queue(), ^{
                                             if (guard)
                                                 callback(result, failure);
                                         });
                                     }];
    } else {
        callback({}, QObject::tr("Screen sharing requires macOS 12.3 or later."));
    }
}
}
