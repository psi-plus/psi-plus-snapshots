// SPDX-License-Identifier: GPL-2.0-or-later
#include "gstscreencapturecontext.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QUuid>

namespace PsiMedia {
namespace {
    const QString portalService       = QStringLiteral("org.freedesktop.portal.Desktop");
    const QString portalPath          = QStringLiteral("/org/freedesktop/portal/desktop");
    const QString screenCastInterface = QStringLiteral("org.freedesktop.portal.ScreenCast");
    QString       token() { return QStringLiteral("psimedia_") + QUuid::createUuid().toString(QUuid::Id128); }
}
void closeScreenCapturePortalObject(const QString &path, const QString &interface)
{
    if (!path.isEmpty())
        QDBusConnection::sessionBus().asyncCall(
            QDBusMessage::createMethodCall(portalService, path, interface, QStringLiteral("Close")));
}
void GstScreenCaptureContext::startPortal()
{
    // Empty parent is supported; a Wayland window must never be identified by an X11 ID.
    step_      = Step::Create;
    auto query = QDBusMessage::createMethodCall(
        portalService, portalPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    query.setArguments({ screenCastInterface });
    const auto generation = generation_;
    auto       watcher    = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(query), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<QVariantMap> reply = *watcher;
        watcher->deleteLater();
        if (generation != generation_)
            return;
        if (reply.isError()) {
            fail(tr("Screen sharing requires a working desktop ScreenCast portal."));
            return;
        }
        sourceTypes_ = reply.value().value(QStringLiteral("AvailableSourceTypes")).toUInt() & 3;
        embedCursor_ = reply.value().value(QStringLiteral("AvailableCursorModes")).toUInt() & 2;
        if (!sourceTypes_) {
            fail(tr("This desktop does not provide display or window capture."));
            return;
        }
        const auto sessionToken = token();
        auto       bus          = QDBusConnection::sessionBus();
        auto       sender       = bus.baseService().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        sessionPath_ = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2").arg(sender, sessionToken);
        bus.connect(portalService, sessionPath_, QStringLiteral("org.freedesktop.portal.Session"),
                    QStringLiteral("Closed"), this, SLOT(portalClosed(QVariantMap)));
        requestPortal(QStringLiteral("CreateSession"), {},
                      { { QStringLiteral("session_handle_token"), sessionToken } });
    });
}
void GstScreenCaptureContext::requestPortal(const QString &method, const QVariantList &arguments, QVariantMap options)
{
    auto          bus    = QDBusConnection::sessionBus();
    const QString handle = token();
    QString       sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    requestPath_ = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handle);
    options.insert(QStringLiteral("handle_token"), handle);
    if (!bus.connect(portalService, requestPath_, QStringLiteral("org.freedesktop.portal.Request"),
                     QStringLiteral("Response"), this, SLOT(portalResponse(uint, QVariantMap, QDBusMessage)))) {
        fail(tr("Unable to open the screen sharing dialog."));
        return;
    }
    auto message      = QDBusMessage::createMethodCall(portalService, portalPath, screenCastInterface, method);
    auto allArguments = arguments;
    allArguments.append(options);
    message.setArguments(allArguments);
    const auto generation = generation_;
    auto       watcher    = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (generation != generation_)
            return;
        if (reply.isError())
            fail(tr("Unable to open the screen sharing dialog: %1").arg(reply.error().message()));
    });
}

void GstScreenCaptureContext::portalResponse(uint response, const QVariantMap &results, const QDBusMessage &message)
{
    if (message.path() != requestPath_ || step_ == Step::Idle)
        return;
    auto bus = QDBusConnection::sessionBus();
    bus.disconnect(portalService, requestPath_, QStringLiteral("org.freedesktop.portal.Request"),
                   QStringLiteral("Response"), this, SLOT(portalResponse(uint, QVariantMap, QDBusMessage)));
    requestPath_.clear();
    if (response != 0) {
        if (response == 1)
            stop();
        else
            fail(tr("The desktop could not start screen sharing."));
        return;
    }
    if (step_ == Step::Create) {
        if (results.value(QStringLiteral("session_handle")).toString() != sessionPath_) {
            fail(tr("The desktop returned an invalid capture session."));
            return;
        }
        step_ = Step::Select;
        QVariantMap options { { QStringLiteral("types"), sourceTypes_ }, { QStringLiteral("multiple"), false } };
        if (embedCursor_)
            options.insert(QStringLiteral("cursor_mode"), uint(2));
        requestPortal(QStringLiteral("SelectSources"), { QVariant::fromValue(QDBusObjectPath(sessionPath_)) }, options);
    } else if (step_ == Step::Select) {
        step_ = Step::Start;
        requestPortal(QStringLiteral("Start"), { QVariant::fromValue(QDBusObjectPath(sessionPath_)), QString() }, {});
    } else if (step_ == Step::Start) {
        const auto streams = results.value(QStringLiteral("streams"));
        if (streams.userType() != qMetaTypeId<QDBusArgument>()) {
            fail(tr("The desktop returned no capture stream."));
            return;
        }
        const auto argument = qvariant_cast<QDBusArgument>(streams);
        if (argument.currentSignature() != QLatin1String("a(ua{sv})")) {
            fail(tr("The desktop returned an invalid capture stream."));
            return;
        }
        uint        node = 0;
        QVariantMap properties;
        argument.beginArray();
        if (!argument.atEnd()) {
            argument.beginStructure();
            argument >> node >> properties;
            argument.endStructure();
        }
        const bool exactlyOne = argument.atEnd();
        argument.endArray();
        if (!node || !exactlyOne) {
            fail(tr("The desktop returned an invalid capture stream."));
            return;
        }
        openPipeWire(node, properties);
    }
}

void GstScreenCaptureContext::openPipeWire(uint node, const QVariantMap &properties)
{
    step_        = Step::Open;
    auto message = QDBusMessage::createMethodCall(portalService, portalPath, screenCastInterface,
                                                  QStringLiteral("OpenPipeWireRemote"));
    message.setArguments({ QVariant::fromValue(QDBusObjectPath(sessionPath_)), QVariantMap {} });
    const auto generation = generation_;
    auto       watcher    = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, node, properties, generation](QDBusPendingCallWatcher *watcher) {
                const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
                watcher->deleteLater();
                if (generation != generation_ || step_ != Step::Open)
                    return;
                if (reply.isError() || !reply.value().isValid()) {
                    fail(tr("Unable to access the selected display or window."));
                    return;
                }
                pipeWireFd_          = reply.value();
                auto       resources = std::make_shared<QDBusUnixFileDescriptor>(pipeWireFd_);
                const auto serial    = properties.value(QStringLiteral("pipewire-serial")).toULongLong();
                const auto target
                    = serial ? QStringLiteral("target-object=%1").arg(serial) : QStringLiteral("path=%1").arg(node);
                const auto pipeline = QStringLiteral("pipewiresrc fd=%1 %2 do-timestamp=true")
                                          .arg(pipeWireFd_.fileDescriptor())
                                          .arg(target);
                step_ = Step::Idle;
                selected(pipeline,
                         properties.value(QStringLiteral("source_type")).toUInt() == 2 ? tr("Selected window")
                                                                                       : tr("Selected display"),
                         std::move(resources));
            });
}

void GstScreenCaptureContext::portalClosed(const QVariantMap &)
{
    sessionPath_.clear(); // the portal has already closed it
    stop();
}
}
