// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gstscreencapturecontext.h"
#include <QSignalSpy>
#include <QTest>

namespace PsiMedia::ScreenCaptureNative {
QList<std::function<void()>> pending;
bool                         available() { return true; }
void                         sources(QObject *receiver, Callback callback)
{
    QPointer<QObject> guard(receiver);
    pending.append([guard, callback] {
        if (guard)
            callback({ { PScreenCaptureSource::Display, "Synthetic display", "videotestsrc is-live=true", {} } }, {});
    });
}
}
using namespace PsiMedia;
class CaptureTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }
    void cleanup() { ScreenCaptureNative::pending.clear(); }
    void selectionIsExplicitAndOpaque()
    {
        GstScreenCaptureContext capture;
        QSignalSpy              sources(&capture, &GstScreenCaptureContext::sourcesAvailable);
        QSignalSpy              ready(&capture, &GstScreenCaptureContext::ready);
        capture.startSelection();
        QCOMPARE(sources.count(), 0);
        QCOMPARE(ready.count(), 0);
        ScreenCaptureNative::pending.takeFirst()();
        QCOMPARE(sources.count(), 1);
        QCOMPARE(ready.count(), 0);
        const auto choices = capture.sources();
        QCOMPARE(choices.size(), 1);
        QVERIFY(!choices.front().id.contains("videotestsrc"));
        capture.choose("unissued-selection-id");
        QCOMPARE(ready.count(), 0);
        capture.choose(choices.front().id);
        QCOMPARE(ready.count(), 1);
        const auto id = ready.front().front().toString();
        QVERIFY(id != choices.front().id);
        auto *element = makeScreenCaptureElement(id);
        QVERIFY(element);
        capture.stop();
        // Permission revocation fences new media graphs even when an old
        // element still retains its source and associated native resources.
        QVERIFY(!makeScreenCaptureElement(id));
        QVERIFY(!makeScreenCaptureElement("psimedia-screen:unissued"));
        gst_object_unref(element);
    }
    void stoppedEnumerationCannotPublishOldChoices()
    {
        GstScreenCaptureContext capture;
        QSignalSpy              sources(&capture, &GstScreenCaptureContext::sourcesAvailable);
        capture.startSelection();
        capture.stop();
        ScreenCaptureNative::pending.takeFirst()();
        QCOMPARE(sources.count(), 0);
        QVERIFY(capture.sources().isEmpty());
        capture.startSelection();
        ScreenCaptureNative::pending.takeFirst()();
        QCOMPARE(sources.count(), 1);
    }
    void graphHoldsResourcesUntilItsSourceIsDestroyed()
    {
        auto               resources = std::make_shared<int>(123);
        std::weak_ptr<int> weak      = resources;
        auto               device    = registerScreenCaptureDevice("videotestsrc is-live=true", resources);
        const auto         id        = device->id;
        auto              *element   = makeScreenCaptureElement(id);
        QVERIFY(element);
        revokeScreenCaptureDevice(id);
        device.reset();
        resources.reset();
        QVERIFY(!weak.expired());
        QVERIFY(!makeScreenCaptureElement(id));
        gst_object_unref(element);
        QVERIFY(weak.expired());
    }
    void contextsDoNotRevokeEachOthersSources()
    {
        GstScreenCaptureContext first, second;
        QSignalSpy              firstReady(&first, &GstScreenCaptureContext::ready),
            secondReady(&second, &GstScreenCaptureContext::ready);
        first.startSelection();
        second.startSelection();
        ScreenCaptureNative::pending.takeFirst()();
        ScreenCaptureNative::pending.takeFirst()();
        first.choose(first.sources().front().id);
        second.choose(second.sources().front().id);
        const auto firstId  = firstReady.front().front().toString();
        const auto secondId = secondReady.front().front().toString();
        QVERIFY(firstId != secondId);
        first.stop();
        QVERIFY(!makeScreenCaptureElement(firstId));
        auto *element = makeScreenCaptureElement(secondId);
        QVERIFY(element);
        gst_object_unref(element);
    }
};
QTEST_GUILESS_MAIN(CaptureTest)
#include "screencapture.moc"
