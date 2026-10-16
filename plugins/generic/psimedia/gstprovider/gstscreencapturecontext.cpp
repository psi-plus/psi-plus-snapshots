// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gstscreencapturecontext.h"
#include <QGuiApplication>
#include <QUuid>
#include <utility>
#ifdef PSIMEDIA_SCREEN_PORTAL
#include <QDBusConnection>
#endif

namespace PsiMedia {
bool screenCaptureWaylandSession()
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))
        || qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland");
}
#ifdef PSIMEDIA_SCREEN_PORTAL
void closeScreenCapturePortalObject(const QString &path, const QString &interface);
#endif
namespace {
    bool hasElement(const char *name)
    {
        auto *factory = gst_element_factory_find(name);
        if (!factory)
            return false;
        gst_object_unref(factory);
        return true;
    }
}
GstScreenCaptureContext::GstScreenCaptureContext(QObject *parent) : QObject(parent) { }
GstScreenCaptureContext::~GstScreenCaptureContext() { stop(); }
bool GstScreenCaptureContext::available()
{
#ifdef PSIMEDIA_SCREEN_PORTAL
    if (screenCaptureWaylandSession())
        return QDBusConnection::sessionBus().isConnected() && hasElement("pipewiresrc");
#endif
#ifdef Q_OS_WIN
    return ScreenCaptureNative::available() && hasElement("d3d11screencapturesrc");
#elif defined(Q_OS_MACOS)
    return ScreenCaptureNative::available() && hasElement("psiscreencapturekitsrc");
#else
    return ScreenCaptureNative::available() && hasElement("ximagesrc");
#endif
}
void GstScreenCaptureContext::startSelection()
{
    QPointer<GstScreenCaptureContext> guard(this);
    const auto                        previousGeneration = generation_;
    stop();
    if (!guard || generation_ != previousGeneration + 1)
        return;
    selecting_            = true;
    const auto generation = generation_;
#ifdef PSIMEDIA_SCREEN_PORTAL
    if (screenCaptureWaylandSession()) {
        startPortal();
        return;
    }
#endif
    ScreenCaptureNative::sources(
        this, [guard, generation](const QList<ScreenCaptureNative::Source> &sources, const QString &error) {
            if (!guard || generation != guard->generation_ || !guard->selecting_)
                return;
            if (!error.isEmpty() || sources.isEmpty()) {
                guard->fail(error.isEmpty() ? tr("No displays or windows are available to share.") : error);
                return;
            }
            for (const auto &source : sources) {
                const auto id = QUuid::createUuid().toString(QUuid::Id128);
                guard->nativeSources_.insert(id, source);
                guard->sources_.append({ source.kind, id, source.label, source.thumbnail });
            }
            emit guard->sourcesAvailable();
        });
}
void GstScreenCaptureContext::choose(const QString &id)
{
    if (!selecting_ || !nativeSources_.contains(id))
        return;
    const auto source = nativeSources_.value(id);
    selected(source.pipeline, source.label);
}
void GstScreenCaptureContext::selected(const QString &pipeline, const QString &label,
                                       std::shared_ptr<const void> resources)
{
    selectedDevice_ = registerScreenCaptureDevice(pipeline, std::move(resources));
    selecting_      = false;
    active_         = true;
    const auto id   = selectedDevice_->id;
    // Clear choices before notifying external code: callbacks can stop/reselect.
    sources_.clear();
    nativeSources_.clear();
    emit ready(id, label);
}
void GstScreenCaptureContext::stop()
{
    ++generation_;
    const bool wasActive    = std::exchange(active_, false);
    const bool wasSelecting = std::exchange(selecting_, false);
    sources_.clear();
    nativeSources_.clear();
    auto device = std::move(selectedDevice_);
    if (device)
        revokeScreenCaptureDevice(device->id);
#ifdef PSIMEDIA_SCREEN_PORTAL
    auto       bus        = QDBusConnection::sessionBus();
    const auto request    = std::exchange(requestPath_, {});
    const auto session    = std::exchange(sessionPath_, {});
    const auto descriptor = std::exchange(pipeWireFd_, {});
    step_                 = Step::Idle;
    if (!request.isEmpty()) {
        bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), request,
                       QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
                       SLOT(portalResponse(uint, QVariantMap, QDBusMessage)));
        closeScreenCapturePortalObject(request, QStringLiteral("org.freedesktop.portal.Request"));
    }
    if (!session.isEmpty())
        bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), session,
                       QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Closed"), this,
                       SLOT(portalClosed(QVariantMap)));
#endif
    if (wasActive || wasSelecting)
        emit stopped();
#ifdef PSIMEDIA_SCREEN_PORTAL
    closeScreenCapturePortalObject(session, QStringLiteral("org.freedesktop.portal.Session"));
#endif
}
void GstScreenCaptureContext::fail(const QString &message)
{
    QPointer<GstScreenCaptureContext> guard(this);
    const auto                        previousGeneration = generation_;
    stop();
    if (guard && generation_ == previousGeneration + 1)
        emit failed(message);
}
}
