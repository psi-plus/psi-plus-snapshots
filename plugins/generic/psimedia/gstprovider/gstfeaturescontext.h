#ifndef PSIMEDIA_GSTFEATURESCONTEXT_H
#define PSIMEDIA_GSTFEATURESCONTEXT_H

#include "psimediaprovider.h"

#include <QMutex>
#include <QPointer>

namespace PsiMedia {

class GstMainLoop;
class DeviceMonitor;

//----------------------------------------------------------------------------
// GstFeaturesContext
//----------------------------------------------------------------------------
class GstFeaturesContext : public QObject, public FeaturesContext {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::FeaturesContext)

    struct Watcher {
        Watcher(int types, bool oneShot, QPointer<QObject> context, std::function<void(const PFeatures &)> &&callback) :
            types(types), oneShot(oneShot), context(context), callback(std::move(callback))
        {
        }
        int                                    types   = 0;
        bool                                   oneShot = true;
        QPointer<QObject>                      context;
        std::function<void(const PFeatures &)> callback;
    };

public:
    QPointer<GstMainLoop> gstLoop;
    DeviceMonitor *       deviceMonitor = nullptr;
    PFeatures             features;
    bool                  updated = false;
    std::list<Watcher>    watchers;

    explicit GstFeaturesContext(GstMainLoop *_gstLoop, DeviceMonitor *deviceMonitor, QObject *parent = nullptr);

    QObject *qobject() override;

    void lookup(int types, QObject *receiver, std::function<void(const PFeatures &)> &&callback) override;
    void monitor(int types, QObject *receiver, std::function<void(const PFeatures &)> &&callback) override;

private slots:
    void watch();

private:
    QList<PDevice> audioOutputDevices();
    QList<PDevice> audioInputDevices();
    QList<PDevice> videoInputDevices();

    void updateDevices();
};

} // namespace PsiMedia

#endif // PSIMEDIA_GSTFEATURESCONTEXT_H
