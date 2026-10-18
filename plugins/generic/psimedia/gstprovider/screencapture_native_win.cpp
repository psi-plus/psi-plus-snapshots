// SPDX-License-Identifier: GPL-2.0-or-later
#include "screencapture_native_p.h"
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QTimer>
#include <dwmapi.h>
#include <string>
#include <windows.h>

namespace PsiMedia::ScreenCaptureNative {
namespace {
// EnumDisplayMonitors requires MONITORENUMPROC (CALLBACK / __stdcall on
// 32-bit Windows). An ordinary function keeps the ABI correct with both
// MSVC and MinGW/MXE, unlike a captureless lambda on some MinGW targets.
BOOL CALLBACK enumerateMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto          &sources = *reinterpret_cast<QList<Source> *>(data);
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info))) {
        const auto rect = info.rcMonitor;
        sources.append({ PScreenCaptureSource::Display,
                         QObject::tr("%1 (%2 × %3)")
                             .arg(QString::fromWCharArray(info.szDevice))
                             .arg(rect.right - rect.left)
                             .arg(rect.bottom - rect.top),
                         QStringLiteral("d3d11screencapturesrc monitor-handle=%1 show-cursor=true")
                             .arg(qulonglong(reinterpret_cast<quintptr>(monitor))) });
    }
    return TRUE;
}
} // namespace

bool available() { return true; }
void sources(QObject *receiver, Callback callback)
{
    const QPointer<QObject> guard(receiver);
    QList<Source>           result;
    EnumDisplayMonitors(nullptr, nullptr, &enumerateMonitor,
                        reinterpret_cast<LPARAM>(&result));
#ifdef _MSC_VER
    // GStreamer's Windows Graphics Capture implementation needs the MSVC SDK.
    // DXGI monitor capture remains available in MinGW builds.
    if (QOperatingSystemVersion::current() >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 18362))
        EnumWindows(
            [](HWND window, LPARAM data) -> BOOL {
                DWORD cloaked = 0;
                DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
                if (!IsWindowVisible(window) || IsIconic(window) || cloaked || GetWindow(window, GW_OWNER)
                    || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                    return TRUE;
                const int length = GetWindowTextLengthW(window);
                if (length <= 0)
                    return TRUE;
                std::wstring title(size_t(length) + 1, L'\0');
                const int    copied = GetWindowTextW(window, &title[0], int(title.size()));
                if (copied > 0)
                    reinterpret_cast<QList<Source> *>(data)->append(
                        { PScreenCaptureSource::Window, QString::fromWCharArray(title.data(), copied),
                          QStringLiteral("d3d11screencapturesrc capture-api=wgc window-handle=%1 show-cursor=true")
                              .arg(qulonglong(reinterpret_cast<quintptr>(window))) });
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&result));
#endif
    if (guard)
        QTimer::singleShot(0, guard, [callback = std::move(callback), result]() { callback(result, {}); });
}
}
