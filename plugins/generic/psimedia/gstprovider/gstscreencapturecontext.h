// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "psimedia/psimediascreencapture.h"
#include "psimediaprovider.h"
#include "screencapture_native_p.h"
#include "screencapturedevice_p.h"
#include <QHash>
#include <QPointer>
#ifdef PSIMEDIA_SCREEN_PORTAL
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#endif

namespace PsiMedia {
class GstScreenCaptureContext : public QObject, public ScreenCaptureContext {
    Q_OBJECT
public:
    explicit GstScreenCaptureContext(QObject *parent = nullptr);
    ~GstScreenCaptureContext() override;
    QObject                    *qobject() override { return this; }
    static bool                 available();
    void                        startSelection() override;
    QList<PScreenCaptureSource> sources() const override { return sources_; }
    void                        choose(const QString &id) override;
    void                        stop() override;
    std::shared_ptr<const void> sourceLease() const override { return selectedDevice_; }

signals:
    void sourcesAvailable();
    void ready(const QString &deviceId, const QString &label);
    void stopped();
    void failed(const QString &message);

private:
    void    fail(const QString &message);
    void    selected(const QString &pipeline, const QString &label, std::shared_ptr<const void> resources = {});
    bool    selecting_  = false;
    bool    active_     = false;
    quint64 generation_ = 0;
    QList<PScreenCaptureSource>                 sources_;
    QHash<QString, ScreenCaptureNative::Source> nativeSources_;
    std::shared_ptr<ScreenCaptureDevice>        selectedDevice_;
#ifdef PSIMEDIA_SCREEN_PORTAL
    enum class Step { Idle, Create, Select, Start, Open };
    Step                    step_ = Step::Idle;
    QString                 requestPath_, sessionPath_;
    QDBusUnixFileDescriptor pipeWireFd_;
    uint                    sourceTypes_ = 3;
    bool                    embedCursor_ = false;
    void                    startPortal();
    void                    requestPortal(const QString &method, const QVariantList &arguments, QVariantMap options);
    void                    openPipeWire(uint node, const QVariantMap &properties);
private slots:
    void portalResponse(uint response, const QVariantMap &results, const QDBusMessage &message);
    void portalClosed(const QVariantMap &details);
#endif
};
}
