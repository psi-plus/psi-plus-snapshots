/*
 * taskbarnotifier.cpp - Taskbar notifications class
 * Copyright (C) 2024  Vitaly Tonkacheyev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "taskbarnotifier.h"

// #include <QApplication> //Maybe it will be needed for macOS to set application icon
#include <QImage>
#include <QPaintDevice>
#include <QPainter>
#include <QPen>
#include <QStaticText>
#include <QString>
#include <QStringList>
#include <QWidget>

#ifdef USE_DBUS
#include "applicationinfo.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QLatin1String>
#include <QVariantMap>
#endif

#ifdef Q_OS_WIN
#include <shobjidl.h>
#include <windows.h>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtWinExtras/qwinfunctions.h>
#endif
#else
#include <QIcon>
#endif

#ifdef USE_DBUS
// UnityLauncher dbus
static const QLatin1String ULAUNCHER_SERV("com.canonical.Unity");
static const QLatin1String ULAUNCHER_PATH("/");
static const QLatin1String ULAUNCHER_IFACE("com.canonical.Unity.LauncherEntry");
// UnityLauncher functions
static const QLatin1String ULAUNCHER_CMD("Update");
#endif

class TaskBarNotifier::Private {
public:
    Private() = default;
    ~Private();

    bool active() const;
    void setIconCount(uint count = 0);
    void restoreDefaultIcon();
    void setParent(QWidget *parent);
#ifdef USE_DBUS
    void setDesktopPath(const QString &appName);
#endif
private:
#ifdef Q_OS_WIN
    void  setTaskBarIcon(const HICON &icon = {});
    HICON makeIconCaption(const QString &number) const;
    HICON getHICONfromQImage(const QImage &image) const;
    void  doFlashTaskbarIcon();
#else
    QIcon setImageCountCaption(uint count = 0);
#ifdef USE_DBUS
    bool checkDBusSeviceAvailable();
    void sendDBusSignal(bool isVisible, uint number = 0);
#endif
#endif

private:
    bool     urgent_ = false;
    bool     active_ = false;
    QWidget *parent_;
#ifdef Q_OS_WIN
    HWND  hwnd_;
    HICON icon_;
#else
    QImage *image_;
#endif
    int devicePixelRatio_;
};

TaskBarNotifier::Private::~Private()
{
#ifdef Q_OS_WIN
    if (icon_)
        DestroyIcon(icon_);
#else
    if (image_)
        delete image_;
#endif
}

bool TaskBarNotifier::Private::active() const { return active_; }

void TaskBarNotifier::Private::setIconCount(uint count)
{
    urgent_ = true;
#ifdef Q_OS_WIN
    setTaskBarIcon(makeIconCaption(QString::number(count)));
    doFlashTaskbarIcon();
#else
#ifdef USE_DBUS
    if (checkDBusSeviceAvailable())
        sendDBusSignal(true, count);
    else
#endif
        parent_->setWindowIcon(setImageCountCaption(count)); // qApp->setWindowIcon(setImageCountCaption(count));
#endif
    active_ = true;
}

void TaskBarNotifier::Private::restoreDefaultIcon()
{
    urgent_ = false;
#ifdef Q_OS_WIN
    setTaskBarIcon();
    doFlashTaskbarIcon();
#else
#ifdef USE_DBUS
    if (checkDBusSeviceAvailable())
        sendDBusSignal(false, 0);
    else
#endif
        parent_->setWindowIcon(
            QIcon(QPixmap::fromImage(*image_))); // qApp->setWindowIcon(QIcon(QPixmap::fromImage(*image_)));
#endif
    active_ = false;
}

void TaskBarNotifier::Private::setParent(QWidget *parent)
{
    parent_           = parent;
    devicePixelRatio_ = parent->devicePixelRatio();
#ifdef Q_OS_WIN
    hwnd_ = reinterpret_cast<HWND>(parent->winId());
#else
    image_ = new QImage(parent->windowIcon().pixmap({ 128, 128 }).toImage());
#endif
}

#ifndef Q_OS_WIN
QIcon TaskBarNotifier::Private::setImageCountCaption(uint count)
{
    auto   imSize    = image_->size() * devicePixelRatio_;
    QImage img       = *image_;
    auto   number    = QString::number(count);
    auto   letters   = number.length();
    auto   text      = (letters < 3) ? QStaticText(number) : QStaticText("∞");
    auto   textDelta = (letters <= 2) ? 3 : 4;

    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    auto font   = QFont(parent_->font().defaultFamily(), imSize.height() / textDelta, QFont::Bold);
    auto fm     = QFontMetrics(font);
    auto fh     = fm.height();
    auto fw     = fm.horizontalAdvance(text.text());
    auto radius = fh / 2;

    Qt::BrushStyle style = Qt::SolidPattern;
    QBrush         brush(Qt::black, style);
    p.setBrush(brush);
    p.setPen(QPen(Qt::NoPen));
    QRect rect(imSize.width() - fw - radius, radius / 4, fw + radius, fh);
    p.drawRoundedRect(rect, radius, radius);

    p.setFont(font);
    p.setPen(QPen(Qt::white));
    auto offset = rect.width() / ((letters + 1) * 2);
    p.drawStaticText(rect.x() + offset, rect.y(), text);

    p.end();
    return QIcon(QPixmap::fromImage(img));
}
#endif

#ifdef USE_DBUS
bool TaskBarNotifier::Private::checkDBusSeviceAvailable()
{
    const auto services = QDBusConnection::sessionBus().interface()->registeredServiceNames().value();
    for (const auto &service : services) {
        if (service.contains(ULAUNCHER_SERV, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

void TaskBarNotifier::Private::sendDBusSignal(bool isVisible, uint number)
{
    auto appName = ApplicationInfo::desktopFileBaseName();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    auto desktopPath_ = QLatin1String("application://%1").arg(appName);
#else
    auto desktopPath_ = QLatin1String("application://%1.desktop").arg(appName);
#endif
    QDBusMessage signal = QDBusMessage::createSignal(ULAUNCHER_PATH, ULAUNCHER_IFACE, ULAUNCHER_CMD);
    signal << desktopPath_;
    QVariantMap args;
    args["count-visible"] = isVisible;
    args["count"]         = number;
    args["urgent"]        = urgent_;
    signal << args;
    QDBusConnection::sessionBus().send(signal);
}

#elif defined(Q_OS_WIN)
void TaskBarNotifier::Private::setTaskBarIcon(const HICON &icon)
{
    if (icon_)
        DestroyIcon(icon_);

    if (icon)
        icon_ = icon;

    ITaskbarList3 *tbList;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3,
                                   reinterpret_cast<void **>(&tbList)))) {
        tbList->SetOverlayIcon(hwnd_, icon_, (icon_) ? L"Incoming events" : 0);
        tbList->Release();
    }
}

HICON TaskBarNotifier::Private::makeIconCaption(const QString &number) const
{
    auto   imSize = QSize(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)) * devicePixelRatio_;
    QImage img(imSize, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QString text      = (number.length() < 3) ? number : "∞";
    auto    letters   = text.length();
    auto    textDelta = (letters <= 2) ? 2 : 3;

    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    auto font   = QFont(parent_->font().defaultFamily(), imSize.height() / textDelta, QFont::Bold);
    auto fm     = QFontMetrics(font);
    auto fh     = fm.height();
    auto radius = fh / 2;

    Qt::BrushStyle style = Qt::SolidPattern;
    QBrush         brush(Qt::black, style);
    p.setBrush(brush);
    p.setPen(QPen(Qt::NoPen));
    QRect rect(0, 0, imSize.width() - 2, imSize.height() - 2);
    p.drawRoundedRect(rect, radius, radius);

    p.setFont(font);
    p.setPen(QPen(Qt::white));
    p.drawText(rect, Qt::AlignCenter, text);

    p.end();
    return std::move(getHICONfromQImage(img));
}

HICON TaskBarNotifier::Private::getHICONfromQImage(const QImage &image) const
{
    if (image.isNull())
        return {};
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    return std::move(QtWin::toHICON(QPixmap::fromImage(image)));
#else
    return std::move(image.toHICON());
#endif
    return {};
}

void TaskBarNotifier::Private::doFlashTaskbarIcon()
{
    FLASHWINFO fi;
    fi.cbSize    = sizeof(FLASHWINFO);
    fi.hwnd      = hwnd_;
    fi.dwFlags   = (urgent_) ? FLASHW_ALL | FLASHW_TIMER : FLASHW_STOP;
    fi.uCount    = 0;
    fi.dwTimeout = 0;
    FlashWindowEx(&fi);
}
#endif

TaskBarNotifier::TaskBarNotifier(QWidget *parent)
{
    d = std::make_unique<Private>(Private());
    d->setParent(parent);
}

TaskBarNotifier::~TaskBarNotifier() = default;

void TaskBarNotifier::setIconCountCaption(int count) { d->setIconCount(count); }

void TaskBarNotifier::removeIconCountCaption() { d->restoreDefaultIcon(); }

bool TaskBarNotifier::isActive() { return d->active(); }
