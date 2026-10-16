// SPDX-License-Identifier: GPL-2.0-or-later
#include "screencapture_native_p.h"
#include <QBuffer>
#include <QGuiApplication>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <atomic>
#ifdef PSIMEDIA_SCREEN_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#ifdef PSIMEDIA_SCREEN_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#undef None
#endif
namespace PsiMedia {
bool screenCaptureWaylandSession();
namespace ScreenCaptureNative {
#ifdef PSIMEDIA_SCREEN_X11
    class X11EnumerationErrors {
        static inline std::atomic<Display *>     display_  = nullptr;
        static inline std::atomic<XErrorHandler> previous_ = nullptr;
        static int                               handle(Display *display, XErrorEvent *event)
        {
            if (display == display_)
                return 0;
            const auto previous = previous_.load();
            return previous ? previous(display, event) : 0;
        }

    public:
        explicit X11EnumerationErrors(Display *display)
        {
            display_  = display;
            previous_ = XSetErrorHandler(handle);
        }
        ~X11EnumerationErrors()
        {
            XSync(display_.load(), False);
            XSetErrorHandler(previous_.load());
            display_ = nullptr;
        }
    };
#endif
    bool available()
    {
#ifdef PSIMEDIA_SCREEN_X11
        return !screenCaptureWaylandSession() && QGuiApplication::platformName() == QLatin1String("xcb");
#else
        return false;
#endif
    }
    void sources(QObject *receiver, Callback callback)
    {
        const QPointer<QObject> guard(receiver);
        QList<Source>           result;
        QString                 error;
#ifdef PSIMEDIA_SCREEN_X11
        if (!available()) {
            error = QObject::tr("Screen sharing is unavailable on this desktop.");
        } else if (Display *display = XOpenDisplay(nullptr)) {
            {
                X11EnumerationErrors errors(display);
                const Window         root = DefaultRootWindow(display);
                const auto append = [&](PScreenCaptureSource::Kind kind, const QString &name, const QString &pipeline,
                                        const QPixmap &preview) {
                    QByteArray thumbnail;
                    if (!preview.isNull()) {
                        QBuffer buffer(&thumbnail);
                        buffer.open(QIODevice::WriteOnly);
                        preview.save(&buffer, "PNG");
                    }
                    result.append({ kind, name, pipeline, thumbnail });
                };
                // XRandR supplies actual root pixel coordinates, including mixed-DPI
                // displays. Qt's logical screen geometry cannot be used as capture pixels.
                QList<QPair<QString, QRect>> monitors;
#ifdef PSIMEDIA_SCREEN_XRANDR
                if (auto resources = XRRGetScreenResourcesCurrent(display, root)) {
                    for (int i = 0; i < resources->noutput; ++i) {
                        auto output = XRRGetOutputInfo(display, resources, resources->outputs[i]);
                        if (!output)
                            continue;
                        if (output->connection == RR_Connected && output->crtc) {
                            auto          crtc = XRRGetCrtcInfo(display, resources, output->crtc);
                            const QString name = QString::fromUtf8(output->name, output->nameLen);
                            if (crtc && crtc->width && crtc->height) {
                                monitors.append({ name, QRect(crtc->x, crtc->y, int(crtc->width), int(crtc->height)) });
                            }
                            if (crtc)
                                XRRFreeCrtcInfo(crtc);
                        }
                        XRRFreeOutputInfo(output);
                    }
                    XRRFreeScreenResources(resources);
                }
#else
                // Keep X11 sharing buildable without the optional XRandR development
                // package. Its standard utility reports the same native pixel rectangles.
                QProcess query;
                query.start(QStringLiteral("xrandr"), { QStringLiteral("--listmonitors") });
                if (query.waitForFinished(500)) {
                    const QRegularExpression geometry(QStringLiteral(
                        "^\\s*\\d+:\\s+\\S+\\s+(\\d+)(?:/\\d+)?x(\\d+)(?:/\\d+)?([+-]\\d+)([+-]\\d+)\\s+(\\S+)"));
                    for (const auto &line : QString::fromUtf8(query.readAllStandardOutput()).split(QLatin1Char('\n'))) {
                        const auto match = geometry.match(line);
                        if (match.hasMatch())
                            monitors.append({ match.captured(5),
                                              QRect(match.captured(3).toInt(), match.captured(4).toInt(),
                                                    match.captured(1).toInt(), match.captured(2).toInt()) });
                    }
                } else {
                    query.kill();
                    query.waitForFinished(100);
                }
#endif
                if (monitors.isEmpty()) {
                    XWindowAttributes attributes;
                    if (XGetWindowAttributes(display, root, &attributes))
                        monitors.append({ QObject::tr("Desktop"), QRect(0, 0, attributes.width, attributes.height) });
                }
                for (const auto &monitor : monitors) {
                    const auto &rect = monitor.second;
                    if (rect.isEmpty() || rect.x() < 0 || rect.y() < 0)
                        continue;
                    const auto source
                        = QStringLiteral(
                              "ximagesrc use-damage=false show-pointer=true startx=%1 starty=%2 endx=%3 endy=%4")
                              .arg(rect.x())
                              .arg(rect.y())
                              .arg(rect.right())
                              .arg(rect.bottom());
                    QPixmap preview;
                    for (auto screen : QGuiApplication::screens()) {
                        if (screen->name() == monitor.first) {
                            preview
                                = screen->grabWindow(0).scaled(210, 125, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                            break;
                        }
                    }
                    append(PScreenCaptureSource::Display,
                           QObject::tr("%1 · %2 × %3").arg(monitor.first).arg(rect.width()).arg(rect.height()), source,
                           preview);
                }
                Atom           actualType;
                int            actualFormat;
                unsigned long  count, remaining;
                unsigned char *data = nullptr;
                if (XGetWindowProperty(display, root, XInternAtom(display, "_NET_CLIENT_LIST_STACKING", False), 0,
                                       65536, False, XA_WINDOW, &actualType, &actualFormat, &count, &remaining, &data)
                        == Success
                    && actualType == XA_WINDOW && actualFormat == 32 && data) {
                    auto ids = reinterpret_cast<Window *>(data);
                    for (unsigned long i = 0; i < count; ++i) {
                        XWindowAttributes attributes;
                        if (!XGetWindowAttributes(display, ids[i], &attributes) || attributes.map_state != IsViewable)
                            continue;
                        unsigned char *name = nullptr;
                        unsigned long  length, rest;
                        if (XGetWindowProperty(display, ids[i], XInternAtom(display, "_NET_WM_NAME", False), 0, 4096,
                                               False, XInternAtom(display, "UTF8_STRING", False), &actualType,
                                               &actualFormat, &length, &rest, &name)
                                != Success
                            || !name)
                            continue;
                        const QString title = actualFormat == 8
                            ? QString::fromUtf8(reinterpret_cast<char *>(name), int(length))
                            : QString();
                        XFree(name);
                        if (title.isEmpty())
                            continue;
                        const QPixmap preview = QGuiApplication::primaryScreen()
                                                    ->grabWindow(WId(ids[i]))
                                                    .scaled(210, 125, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        append(PScreenCaptureSource::Window, title,
                               QStringLiteral("ximagesrc use-damage=false show-pointer=true xid=%1")
                                   .arg(qulonglong(ids[i])),
                               preview);
                    }
                }
                if (data)
                    XFree(data);
            }
            XCloseDisplay(display);
        } else {
            error = QObject::tr("Unable to connect to the display.");
        }
#else
        error = QObject::tr("Screen sharing is unavailable on this desktop.");
#endif
        if (guard)
            QTimer::singleShot(0, guard, [callback = std::move(callback), result, error] { callback(result, error); });
    }
}
}
