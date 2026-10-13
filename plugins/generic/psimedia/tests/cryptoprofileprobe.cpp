// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gstprovider.h"
#include "securertpcontext.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <srtp2/srtp.h>
#include <thread>

// Intercept the actual backend call, rather than substituting the profile
// query under test. A blocked libSRTP must not block constructor/UI queries.
static QSemaphore            entered, release;
static std::atomic<int>      contexts { 0 };
static std::atomic<bool>     onUiThread { false };
extern "C" srtp_err_status_t __real_srtp_create(srtp_t *, const srtp_policy_t *);
extern "C" srtp_err_status_t __wrap_srtp_create(srtp_t *context, const srtp_policy_t *policy)
{
    if (QThread::currentThread() == qApp->thread())
        onUiThread = true;
    if (contexts.fetch_add(1) == 0) {
        entered.release();
        release.acquire();
    }
    return __real_srtp_create(context, policy);
}

static void require(bool condition, const char *message)
{
    if (!condition)
        qFatal("%s", message);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    auto         provider = std::make_unique<PsiMedia::GstProvider>();
    require(provider->isInitialized(), "GStreamer did not initialize");
    require(entered.tryAcquire(1, 5000), "SRTP probe did not start on the worker");
    require(!onUiThread, "SRTP probe ran on the GUI thread");
    if (argc > 1) {
        // Drop a FeaturesContext before its queued device-monitor startup can
        // run: the crypto probe is deliberately keeping the loop occupied.
        std::unique_ptr<PsiMedia::FeaturesContext> discarded(provider->createFeatures());
        discarded.reset();
        std::thread unblock([] { release.release(); });
        provider.reset();
        unblock.join();
        QCoreApplication::processEvents(); // discard queued publication to destroyed provider
        return 0;
    }

    std::unique_ptr<PsiMedia::FeaturesContext> features(provider->createFeatures());
    int                                        featureUpdates = 0;
    features->lookup(0xff, &app, [&](const PsiMedia::PFeatures &) { ++featureUpdates; });
    features->monitor(0xff, &app, [&](const PsiMedia::PFeatures &) { ++featureUpdates; });
    bool published = false;
    QObject::connect(provider.get(), &PsiMedia::GstProvider::secureRtpProfilesChanged, &app, [&] {
        require(QThread::currentThread() == app.thread(), "Profile publication ran off the GUI thread");
        published = true;
    });
    bool heartbeat = false;
    QTimer::singleShot(0, &app, [&] { heartbeat = true; });
    for (int i = 0; i < 10000; ++i)
        require(provider->supportedSecureRtpProfiles().isEmpty(), "Pending probe returned usable profiles");
    QCoreApplication::processEvents();
    require(heartbeat && !published, "UI did not remain responsive while probing");
    require(featureUpdates == 0, "Backend announced features before completing its crypto probe");
    release.release();
    QElapsedTimer deadline;
    deadline.start();
    while (!published && deadline.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(published, "Profile result was not published");
    require(featureUpdates > 0, "Existing feature monitors were not notified");
    const auto profiles = provider->supportedSecureRtpProfiles();
    require(!profiles.isEmpty(), "Runtime libSRTP supplied no profiles");
    const auto probed = contexts.load();
    for (int i = 0; i < 10000; ++i)
        require(provider->supportedSecureRtpProfiles() == profiles, "Cached profiles changed");
    require(PsiMedia::SrtpAssociation::supportedProfiles() == profiles, "Association cache differs from provider");
    require(contexts.load() == probed, "Repeated capability queries created new SRTP contexts");
    require(!onUiThread, "A capability query entered libSRTP on the GUI thread");
    return 0;
}
