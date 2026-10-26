/* SPDX-License-Identifier: Apache-2.0 */
#include "qiteaudio.h"
#include "qitemedia_p.h"
#include "qitevideo.h"
#include "qitevideoframe_p.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSlider>
#include <QSplitter>
#include <QStyleOptionSlider>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <limits>
#include <thread>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QVideoSink>
#else
#include <QVideoProbe>
#include <QVideoSurfaceFormat>
#endif

class TextEdit : public QTextEdit {
public:
    int paints     = 0;
    int paintDepth = 0, maxPaintDepth = 0;
    TextEdit()
    {
        setReadOnly(true);
        setCursorWidth(0);
        resize(640, 480);
    }
    void paintEvent(QPaintEvent *event) override
    {
        ++paints;
        maxPaintDepth = qMax(maxPaintDepth, ++paintDepth);
        QTextEdit::paintEvent(event);
        --paintDepth;
    }
};

class VideoController : public ITEVideoController {
public:
    int    draws = 0;
    QRectF objectRect;
    using ITEVideoController::ITEVideoController;
    void drawITE(QPainter *p, const QRectF &rect, int position, const QTextFormat &format) override
    {
        ++draws;
        objectRect = rect;
        ITEVideoController::drawITE(p, rect, position, format);
    }
};

class PaintCounter : public QObject {
public:
    int  paints = 0;
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};

class TestMediaSlider : public ITEMediaSlider {
public:
    QPoint positionForValue(int value) const
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        option.sliderPosition = value;
        return style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this).center();
    }
};

class StateOpener : public ITEMediaOpener {
public:
    ITEMediaState state;
    int           opens = 0;
    QIODevice    *open(QUrl &) override
    {
        ++opens;
        return nullptr;
    }
    void           close(QIODevice *) override { }
    QVariant       metadata(const QUrl &) override { return QVariantMap(); }
    ITEMediaState *mediaState(const QUrl &) override { return &state; }
};

// A real opener-owned stream, unlike StateOpener's URL-only fixture.
class BufferOpener : public ITEMediaOpener {
public:
    int                 opens = 0, closes = 0;
    QPointer<QIODevice> device;
    QIODevice          *open(QUrl &) override
    {
        ++opens;
        auto  buffer = new QBuffer(this);
        QFile file(QStringLiteral(QITE_TEST_VIDEO));
        if (file.open(QIODevice::ReadOnly))
            buffer->setData(file.readAll());
        buffer->open(QIODevice::ReadOnly);
        device = buffer;
        return buffer;
    }
    void close(QIODevice *stream) override
    {
        ++closes;
        delete stream;
    }
    QVariant metadata(const QUrl &) override { return {}; }
};

class AudioController : public ITEAudioController {
public:
    using ITEAudioController::ITEAudioController;
    using ITEAudioController::mouseEvent;
};

class VideoTest : public QObject {
    Q_OBJECT
    static void moveMouse(QWidget *widget, const QPoint &position)
    {
        // QTest::mouseMove may only warp the cursor on some QPA plugins;
        // Wayland cannot warp it. Deliver the actual mouse event explicitly.
        QMouseEvent event(QEvent::MouseMove, position, widget->mapTo(widget->window(), position),
                          widget->mapToGlobal(position), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    }
private slots:
    void initTestCase() { qRegisterMetaType<QVideoFrame>(); }

    void mediaStateAndLayers()
    {
        ITEMediaState state;
        QSignalSpy    changed(&state, &ITEMediaState::changed);
        state.setCachedRanges({ { .5, .7 },
                                { .1, .3 },
                                { .2, .5 },
                                { -1, .1 },
                                { .8, 2 },
                                { .9, .8 },
                                { std::numeric_limits<qreal>::quiet_NaN(), .5 } });
        QCOMPARE(state.cachedRanges(), (ITEMediaState::Ranges { { 0, .7 }, { .8, 1 } }));
        QCOMPARE(changed.count(), 1);
        state.setCachedRanges({ { 0, .7 }, { .8, 1 } });
        QCOMPARE(changed.count(), 1);
        state.setCachedRanges({ { 0, .5 }, { .8, 1 } }); // Eviction must shrink, not union forever.
        state.setDownload(true, 5);
        QCOMPARE(state.downloadProgress(), qreal(1));
        state.setDownload(false);
        QImage image(100, 12, QImage::Format_RGB32);
        image.fill(Qt::black);
        {
            QPainter painter(&image);
            itePaintMediaProgress(&painter, image.rect(), &state, .25, Qt::white);
        }
        QCOMPARE(image.pixelColor(10, 6), QColor(Qt::white)); // Playback stays visible.
        QVERIFY(image.pixelColor(10, 1).blue() > 80);         // Cache remains visible around it.
        QCOMPARE(image.pixelColor(60, 6), QColor(Qt::black)); // Real gap in availability.
        QVERIFY(image.pixelColor(90, 6).blue() > 80);
        image.fill(Qt::black);
        {
            QPainter painter(&image);
            itePaintMediaProgress(&painter, image.rect(), &state, .25, Qt::white, true);
        }
        QCOMPARE(image.pixelColor(90, 6), QColor(Qt::white));
        state.setDownload(false, -1, "network error");
        QVERIFY(iteMediaDownloadText(&state).contains("network error"));
    }

    void videoSliderClickSeeksToAbsolutePosition()
    {
        TestMediaSlider slider;
        slider.setRange(0, 10000);
        slider.setPageStep(25); // A normal QSlider moves only one page per groove click.
        slider.resize(520, 35);
        slider.show();
        QTRY_VERIFY(slider.isVisible());

        const auto clickAt = [&slider](int percent) {
            // Stay clear of the handle and use the center of the groove.
            QTest::mouseClick(&slider, Qt::LeftButton, Qt::NoModifier, slider.positionForValue(percent * 100));
            QVERIFY2(qAbs(slider.value() - percent * 100) <= 100,
                     qPrintable(QStringLiteral("clicked %1% but slider value is %2")
                                    .arg(percent).arg(slider.value())));
        };
        clickAt(80);
        clickAt(20);
        clickAt(95);
        // 95% of a two-hour video must be about 114 minutes, not 5 seconds
        // nearer to the previous position.
        const qint64 duration = 120LL * 60 * 1000;
        QVERIFY(qAbs(duration * slider.value() / slider.maximum() - duration * 95 / 100) <= 72000);

        // Normal handle clicks still enter Qt's drag mode.
        slider.setValue(5000); // Handle is centered across desktop styles.
        QSignalSpy pressed(&slider, &QSlider::sliderPressed);
        QTest::mousePress(&slider, Qt::LeftButton, Qt::NoModifier,
                          slider.rect().center());
        QCOMPARE(pressed.count(), 1);
        QVERIFY(slider.isSliderDown());
        QTest::mouseRelease(&slider, Qt::LeftButton);
        QVERIFY(!slider.isSliderDown());
    }

    void videoDownloadAndCache()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        StateOpener     opener;
        const QUrl      url("test:video");
        video.insert(url, &opener);
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        QVERIFY(!edit.findChild<QToolButton *>("qite-video-download"));
        edit.clear();
        video.insert(url, &opener, QSize(320, 220), true);
        QTRY_VERIFY(edit.findChild<QToolButton *>("qite-video-download"));
        QTest::qWait(30);
        auto       download = edit.findChild<QToolButton *>("qite-video-download");
        QSignalSpy requested(&video, &ITEVideoController::downloadRequested);
        QSignalSpy changed(edit.document(), &QTextDocument::contentsChanged);
        QSignalSpy resized(edit.document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged);
        edit.paints = video.draws = 0;
        opener.state.setCachedRanges({ { 0, .1 }, { .8, 1 } });
        opener.state.setDownload(true, .2);
        QTest::qWait(30);
        QVERIFY(!download->isEnabled());
        QCOMPARE(changed.count(), 0);
        QCOMPARE(resized.count(), 0);
        QCOMPARE(edit.paints, 0);
        QCOMPARE(video.draws, 0);
        download->click();
        QCOMPARE(requested.count(), 0);
        opener.state.setDownload(false);
        download->click();
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toUrl(), url);
        QCOMPARE(opener.opens, 0); // Saving does not start playback.
        edit.findChild<QToolButton *>("qite-video-fullscreen-button")->click();
        QTRY_VERIFY(download->window()->isFullScreen());
        QPointer<QWidget> fullscreen = download->window();
        download->click();
        QCOMPARE(requested.count(), 2);
        QCOMPARE(opener.opens, 0);
        edit.clear();
        QTest::qWait(20);
        opener.state.setCachedRanges({}); // Detached player still observes the shared state.
        fullscreen->close();
        QTRY_VERIFY(fullscreen.isNull());
        opener.state.setCachedRanges({ { 0, .5 } }); // Closed player disconnects safely.
    }

    void audioDownloadAndCache()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        AudioController audio(&text, &edit);
        StateOpener     opener;
        const QUrl      url("test:audio");
        const auto      normal  = audio.makeFormat(url, &opener);
        const auto      enabled = audio.makeFormat(url, &opener, true);
        const auto      small   = audio.intrinsicSize(edit.document(), 0, normal);
        const auto      large   = audio.intrinsicSize(edit.document(), 0, enabled);
        QVERIFY(large.width() > small.width());
        QCOMPARE(large.height(), small.height());
        text.insert(InteractiveTextFormat(enabled));
        QSignalSpy metadata(edit.document(), &QTextDocument::contentsChanged);
        edit.show();
        QTRY_VERIFY(metadata.count() > 0); // Wait for the existing asynchronous metadata update.
        QTest::qWait(20);
        QSignalSpy changed(edit.document(), &QTextDocument::contentsChanged);
        opener.state.setCachedRanges({ { .2, .5 } });
        QTest::qWait(20);
        QCOMPARE(changed.count(), 0);
        QSignalSpy requested(&audio, &ITEAudioController::downloadRequested);
        auto       selected = text.findElement(InteractiveTextFormat::id(enabled));
        QVERIFY(!selected.isNull());
        InteractiveTextElementController::Event click { nullptr, InteractiveTextElementController::EventType::Click,
                                                        QPoint(int(large.width() - large.height() / 2),
                                                               int(large.height() / 2)) };
        audio.mouseEvent(click, {}, selected);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toUrl(), url);
        opener.state.setDownload(true, .4);
        audio.mouseEvent(click, {}, selected);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(opener.opens, 0);
        QVERIFY(edit.findChildren<QMediaPlayer *>().isEmpty());
        edit.clear();
        opener.state.setCachedRanges({});
    }

    void queuedFrames()
    {
        // Exercise producer-to-GUI delivery while the GUI is busy, then destroy
        // the receiver with another delivery pending.
        auto output = new ITEVideoFrameWidget;
        output->resize(160, 90);
        output->show();
        std::thread producer([output] {
            for (int i = 0; i < 240; ++i) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QVideoFrame frame(QVideoFrameFormat(QSize(160, 90), QVideoFrameFormat::Format_BGRX8888));
                frame.map(QVideoFrame::WriteOnly);
                memset(frame.bits(0), i, size_t(frame.mappedBytes(0)));
                frame.unmap();
                emit output->videoSink()->videoFrameChanged(frame);
#else
                QVideoFrame frame(160 * 90 * 4, QSize(160, 90), 160 * 4, QVideoFrame::Format_RGB32);
                frame.map(QAbstractVideoBuffer::WriteOnly);
                memset(frame.bits(), i, size_t(frame.mappedBytes()));
                frame.unmap();
                emit output->videoSurface()->frameReady(frame, QSize(160, 90), false, {});
#endif
            }
        });
        producer.join();
        QTRY_COMPARE(output->grab().toImage().pixelColor(QPoint(80, 45)), QColor(239, 239, 239));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        emit output->videoSink()->videoFrameChanged(QVideoFrame());
#else
        emit output->videoSurface()->frameReady(QVideoFrame(), {}, false, {});
#endif
        delete output;
        QCoreApplication::processEvents();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void surfaceViewport()
    {
        ITEVideoFrameWidget output;
        output.resize(80, 80);
        QVideoSurfaceFormat format(QSize(4, 2), QVideoFrame::Format_RGB32);
        format.setViewport(QRect(1, 0, 2, 2));
        format.setScanLineDirection(QVideoSurfaceFormat::BottomToTop);
        QVERIFY(output.videoSurface()->start(format));
        QImage image(4, 2, QImage::Format_RGB32);
        image.fill(Qt::red); // Cropped pixels must not be displayed.
        image.setPixelColor(1, 0, Qt::green);
        image.setPixelColor(2, 0, Qt::green);
        image.setPixelColor(1, 1, Qt::cyan);
        image.setPixelColor(2, 1, Qt::cyan);
        output.videoSurface()->present(QVideoFrame(image));
        output.show();
        // Sample away from the interpolated boundary between the two rows.
        QTRY_COMPARE(output.grab().toImage().pixelColor(20, 10), QColor(Qt::cyan));
        QCOMPARE(output.grab().toImage().pixelColor(20, 70), QColor(Qt::green));
    }
#endif

    void fullscreen()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        edit.insertPlainText("\n" + QString("chat\n").repeated(100));
        edit.moveCursor(QTextCursor::Start);
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto frame = edit.findChild<ITEVideoFrameWidget *>();
        QCOMPARE(frame->cursor().shape(), Qt::ArrowCursor);
        auto inlinePlayer = frame->parentWidget();
        auto button       = inlinePlayer->findChild<QToolButton *>(QStringLiteral("qite-video-fullscreen-button"));
        auto controls     = inlinePlayer->findChild<QWidget *>(QStringLiteral("qite-video-controls"));
        QVERIFY(button);
        QSignalSpy changed(edit.document(), &QTextDocument::contentsChanged);
        auto       fullscreenWindow = []() -> QWidget       *{
            for (auto widget : QApplication::topLevelWidgets())
                if (widget->objectName() == QLatin1String("qite-video-fullscreen") && widget->isVisible())
                    return widget;
            return nullptr;
        };
        button->click();
        QTRY_VERIFY(fullscreenWindow());
        QPointer<QWidget> full = fullscreenWindow();
        QVERIFY(full->isFullScreen());
        QVERIFY(QTest::qWaitForWindowExposed(full));
        QCOMPARE(frame->window(), full.data());
        QTRY_COMPARE(frame->geometry(), full->rect());
        QVERIFY(full->rect().contains(controls->geometry()));
        QVERIFY(controls->isVisible());
        QVERIFY(!edit.viewport()->testAttribute(Qt::WA_NativeWindow));
        moveMouse(frame, QPoint(10, 10));
        // Wayland may send delayed enter/move events after configuring the
        // fullscreen window; each correctly restarts the two-second idle timer.
        QTRY_VERIFY_WITH_TIMEOUT(controls->isHidden(), 8000);
        moveMouse(frame, QPoint(30, 30));
        QTRY_VERIFY(controls->isVisible());
        QCOMPARE(frame->cursor().shape(), Qt::ArrowCursor);
        // Exit must work even while the controls are hidden.
        QTRY_VERIFY_WITH_TIMEOUT(controls->isHidden(), 8000);
        QTest::keyClick(full, Qt::Key_Escape);
        QTRY_VERIFY(full.isNull());
        QCOMPARE(frame->parentWidget(), inlinePlayer);
        QVERIFY(controls->isVisible());
        QCOMPARE(changed.count(), 0);
        QVERIFY(!edit.viewport()->testAttribute(Qt::WA_NativeWindow));
        button->click();
        QTRY_VERIFY(fullscreenWindow());
        full = fullscreenWindow();
        button->click();
        QTRY_VERIFY(full.isNull());
        button->click();
        QTRY_VERIFY(fullscreenWindow());
        full = fullscreenWindow();
        full->close();
        QTRY_VERIFY(full.isNull());
        button->click();
        QTRY_VERIFY(fullscreenWindow());
        full = fullscreenWindow();
        full->showNormal(); // Native/window-manager exit restores inline too.
        QTRY_VERIFY(full.isNull());
        QTest::mouseDClick(frame, Qt::LeftButton);
        QTRY_VERIFY(fullscreenWindow());
        full = fullscreenWindow();
        QTest::mouseDClick(frame, Qt::LeftButton);
        QTRY_VERIFY(full.isNull());
        button->click();
        QTRY_VERIFY(fullscreenWindow());
        full = fullscreenWindow();
        edit.clear(); // Pruning the chat must NOT close an undocked player.
        QTest::qWait(30); // Let the document's queued synchronization orphan the player.
        QTRY_VERIFY(!full.isNull());
        QVERIFY(full->isFullScreen());
        QCOMPARE(frame->window(), full.data());
        QVERIFY(edit.findChildren<ITEVideoFrameWidget *>().isEmpty());
        QTest::keyClick(full, Qt::Key_Escape);
        QTRY_VERIFY(full && !full->isFullScreen()); // No inline anchor: keep a normal window.
        QCOMPARE(full->objectName(), QStringLiteral("qite-video-window"));
        full->close();
        QTRY_VERIFY(full.isNull());
    }

    void separateWindowOutlivesChatMessage()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        StateOpener     opener;
        video.insert(QUrl(QStringLiteral("test:video")), &opener);
        edit.insertPlainText("\n" + QString("chat\n").repeated(150));
        edit.moveCursor(QTextCursor::Start);
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto frame = edit.findChild<ITEVideoFrameWidget *>();
        auto inlinePlayer = frame->parentWidget();
        auto detach = inlinePlayer->findChild<QToolButton *>(QStringLiteral("qite-video-window-button"));
        QVERIFY(detach);
        QCOMPARE(detach->toolTip(), QStringLiteral("Open in separate window"));

        auto play = inlinePlayer->findChild<QToolButton *>(QStringLiteral("qite-video-play"));
        QVERIFY(play);
        play->click();
        QPointer<QMediaPlayer> media = inlinePlayer->findChild<QMediaPlayer *>();
        QVERIFY(media);
        QCOMPARE(opener.opens, 1);

        detach->click();
        auto popup = frame->window();
        QPointer<QWidget> window = popup;
        QVERIFY(popup != &edit);
        QCOMPARE(popup->objectName(), QStringLiteral("qite-video-window"));
        QVERIFY(popup->isVisible());
        QVERIFY(!popup->isFullScreen());
        QCOMPARE(popup->findChild<QMediaPlayer *>(), media.data());
        QCOMPARE(opener.opens, 1);

        // Scrolling the original message away must not hide or pause the window.
        edit.verticalScrollBar()->setValue(edit.verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
        QVERIFY(window && window->isVisible());
        QCOMPARE(media->parent(), static_cast<QObject *>(inlinePlayer)); // The decoder stays with its VideoPlayer.
        QCOMPARE(opener.opens, 1);

        // The pop-out control toggles back to inline when its message survives.
        detach->click();
        QTRY_VERIFY(window.isNull());
        QCOMPARE(frame->parentWidget(), inlinePlayer);
        QCOMPARE(inlinePlayer->parentWidget(), edit.viewport());
        QCOMPARE(inlinePlayer->findChild<QMediaPlayer *>(), media.data());

        // Pop out once more, then delete the entire chat log.
        edit.verticalScrollBar()->setValue(0);
        QTRY_VERIFY(inlinePlayer->isVisible());
        detach->click();
        popup = frame->window();
        window = popup;
        QVERIFY(popup != &edit);
        edit.clear();
        QTRY_VERIFY(window && window->isVisible());
        QTRY_VERIFY(!detach->isEnabled()); // In-memory message anchor has been removed.
        QCOMPARE(frame->window(), window.data());
        QCOMPARE(window->findChild<QMediaPlayer *>(), media.data());
        QCOMPARE(opener.opens, 1);
        QVERIFY(edit.findChildren<ITEVideoFrameWidget *>().isEmpty());

        window->close();
        QTRY_VERIFY(window.isNull());
        QVERIFY(media.isNull()); // Window deletion releases decoder and stream.
    }

    void detachedWindowSurvivesControllerDestruction()
    {
        StateOpener opener;
        QPointer<QWidget> detached;
        QPointer<QMediaPlayer> media;
        QPointer<QToolButton>  download;
        {
            TextEdit edit;
            InteractiveText text(&edit);
            {
                VideoController video(&text);
                video.insert(QUrl(QStringLiteral("test:video")), &opener, QSize(320, 220), true);
                edit.show();
                QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
                auto widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
                auto play = widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"));
                QVERIFY(play);
                play->click();
                media = widget->findChild<QMediaPlayer *>();
                download = widget->findChild<QToolButton *>(QStringLiteral("qite-video-download"));
                QVERIFY(download && download->isEnabled());
                QVERIFY(media);
                auto detach = widget->findChild<QToolButton *>(QStringLiteral("qite-video-window-button"));
                QVERIFY(detach);
                detach->click();
                detached = widget->window();
                QVERIFY(detached && detached->isVisible());
            } // The controller no longer owns the detached player.
            QVERIFY(detached && detached->isVisible());
            QVERIFY(download && !download->isEnabled());
            opener.state.setCachedRanges({ { 0, 1 } });
            QVERIFY(!download->isEnabled());
        } // Even destroying QTextEdit leaves the top-level video window.
        QVERIFY(detached && detached->isVisible());
        QVERIFY(media);
        detached->close();
        QTRY_VERIFY(detached.isNull());
        QVERIFY(media.isNull());
    }

    void separateWindowCanToggleFullscreenWithoutReopening()
    {
        TextEdit edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        StateOpener opener;
        video.insert(QUrl(QStringLiteral("test:video")), &opener);
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
        widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"))->click();
        auto media = widget->findChild<QMediaPlayer *>();
        QVERIFY(media);
        auto detach = widget->findChild<QToolButton *>(QStringLiteral("qite-video-window-button"));
        auto fullscreen = widget->findChild<QToolButton *>(QStringLiteral("qite-video-fullscreen-button"));
        QVERIFY(detach);
        QVERIFY(fullscreen);
        detach->click();
        QPointer<QWidget> window = widget->window();
        QVERIFY(window && window->isVisible());
        fullscreen->click();
        QTRY_VERIFY(window->isFullScreen());
        QCOMPARE(window->objectName(), QStringLiteral("qite-video-fullscreen"));
        QCOMPARE(window->findChild<QMediaPlayer *>(), media);
        fullscreen->click();
        QTRY_VERIFY(!window->isFullScreen());
        QCOMPARE(window->objectName(), QStringLiteral("qite-video-window"));
        QCOMPARE(window->findChild<QMediaPlayer *>(), media);
        QCOMPARE(opener.opens, 1);
        window->close();
        QTRY_VERIFY(window.isNull());
        QCOMPARE(widget->parentWidget(), edit.viewport());
    }

    void detachedPlaybackContinuesAfterPruning()
    {
        if (!qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK"))
            QSKIP("Enable QITE_TEST_PLAYBACK with a working WebM decoder");

        TextEdit edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
        auto play = widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"));
        auto popout = widget->findChild<QToolButton *>(QStringLiteral("qite-video-window-button"));
        QVERIFY(play);
        QVERIFY(popout);
        play->click();
        QPointer<QMediaPlayer> player = widget->findChild<QMediaPlayer *>();
        QVERIFY(player);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 200, 10000);
        popout->click();
        QPointer<QWidget> window = widget->window();
        QVERIFY(window && window->isVisible());
        edit.clear();
        QTRY_VERIFY(window && window->isVisible());
        const qint64 previous = player->position();
        QTRY_VERIFY_WITH_TIMEOUT(player && player->position() > previous + 200, 5000);
        window->close();
        QTRY_VERIFY(window.isNull());
        QVERIFY(player.isNull());
    }

    void detachedStreamSurvivesChatAndClosesOnce()
    {
        BufferOpener           opener;
        QPointer<QWidget>      detached;
        QPointer<QMediaPlayer> media;
        {
            TextEdit        edit;
            InteractiveText text(&edit);
            VideoController video(&text);
            video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)), &opener);
            edit.show();
            QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
            auto widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
            widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"))->click();
            media = widget->findChild<QMediaPlayer *>();
            widget->findChild<QToolButton *>(QStringLiteral("qite-video-window-button"))->click();
            detached = widget->window();
            QCOMPARE(opener.opens, 1);
            QVERIFY(opener.device);
        }
        QVERIFY(detached && media && opener.device);
        QCOMPARE(opener.closes, 0);
        detached->close();
        QTRY_VERIFY(detached.isNull());
        QVERIFY(media.isNull());
        QVERIFY(opener.device.isNull());
        QCOMPARE(opener.closes, 1);
    }

    void openerCanDieBeforeDetachedWindow()
    {
        auto                   opener = new BufferOpener;
        QPointer<QWidget>      detached;
        QPointer<QWidget>      widget;
        QPointer<QMediaPlayer> media;
        QPointer<QIODevice>    device;
        {
            TextEdit        edit;
            InteractiveText text(&edit);
            VideoController video(&text);
            video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)), opener);
            edit.show();
            QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
            widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
            widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"))->click();
            widget->findChild<QToolButton *>(QStringLiteral("qite-video-window-button"))->click();
            detached = widget->window();
            media    = widget->findChild<QMediaPlayer *>();
            device   = opener->device;
        }
        delete opener; // PsiAccount deletes its FileSharingDeviceOpener after closing chats.
        QVERIFY(detached && widget);
        QVERIFY(media.isNull());
        QVERIFY(device.isNull());
        QVERIFY(!widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"))->isEnabled());
        detached->close();
        QTRY_VERIFY(detached.isNull());
    }

    void openerCanDieBeforePlayerMaterialization()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        {
            BufferOpener opener;
            video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)), &opener);
        }
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
        auto play   = widget->findChild<QToolButton *>(QStringLiteral("qite-video-play"));
        QVERIFY(play && !play->isEnabled());
        play->click();
        QVERIFY(!widget->findChild<QMediaPlayer *>());
    }

    void resizeStress()
    {
        class Window : public QMainWindow {
        public:
            QSplitter *splitter = nullptr;
            void       resizeEvent(QResizeEvent *event) override
            {
                QMainWindow::resizeEvent(event);
                if (splitter && isVisible()) {
                    const auto sizes = splitter->sizes();
                    if (sizes.size() == 2)
                        splitter->setSizes({ 211, sizes[1] + sizes[0] - 211 });
                }
            }
        } window;
        window.splitter = new QSplitter(&window);
        window.setCentralWidget(window.splitter);
        window.splitter->addWidget(new QLabel("Roster"));
        auto host = new QWidget;
        window.splitter->addWidget(host);
        for (int i = 0; i < 5; ++i) {
            auto layout = new QVBoxLayout(host);
            auto child  = new QFrame;
            child->setFrameStyle(QFrame::StyledPanel);
            layout->addWidget(child);
            host = child;
        }
        auto layout = new QVBoxLayout(host);
        auto edit   = new TextEdit;
        layout->addWidget(edit);
        InteractiveText text(edit);
        VideoController video(&text);
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        edit->insertPlainText("\n" + QString("chat\n").repeated(200));
        edit->moveCursor(QTextCursor::Start);
        window.resize(900, 600);
        window.show();
        QTRY_VERIFY(edit->findChild<ITEVideoFrameWidget *>());
        if (qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK")) {
            edit->findChild<QToolButton *>(QStringLiteral("qite-video-play"))->click();
            const auto player = edit->findChild<QMediaPlayer *>();
            QVERIFY(player);
            QTRY_VERIFY_WITH_TIMEOUT(player->position() > 0, 10000);
        }
        for (int i = 0; i < 100; ++i) {
            window.resize(750 + i * 13 % 400, 450 + i * 19 % 250);
            QTest::qWait(3);
        }
        // A native video window promotes the entire QTextEdit ancestor chain
        // to native windows, allowing Wayland exposes during resize/layout.
        QVERIFY(!edit->viewport()->testAttribute(Qt::WA_NativeWindow));
        QCOMPARE(edit->maxPaintDepth, 1);
    }

    void placement_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("left") << 0;
        QTest::newRow("center") << 1;
        QTest::newRow("rtl") << 2;
        QTest::newRow("table") << 3;
        QTest::newRow("mixed-rtl") << 4;
    }

    void placement()
    {
        QFETCH(int, mode);
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        if (mode == 1)
            edit.setStyleSheet(QStringLiteral("QTextEdit { background: #f5f6f8; font-size: 14px; }"));
        if (mode == 3) {
            auto cursor = edit.textCursor();
            auto table  = cursor.insertTable(2, 2);
            edit.setTextCursor(table->cellAt(1, 1).firstCursorPosition());
        }
        QTextBlockFormat block;
        if (mode == 1)
            block.setAlignment(Qt::AlignHCenter);
        if (mode == 2 || mode == 4)
            block.setLayoutDirection(Qt::RightToLeft);
        edit.textCursor().setBlockFormat(block);
        if (mode == 4)
            edit.insertPlainText(QStringLiteral("\u05d0\u05d1\u05d2 abc "));
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        if (mode == 4)
            edit.insertPlainText(QStringLiteral(" \u05d0\u05d1\u05d2 def"));
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto widget = edit.findChild<ITEVideoFrameWidget *>()->parentWidget();
        // Compare with the rect actually supplied by QTextDocument's renderer,
        // rather than duplicating the geometry calculation in the assertion.
        const QPoint scroll(edit.horizontalScrollBar()->value(), edit.verticalScrollBar()->value());
        QCOMPARE(widget->geometry(), video.objectRect.toRect().translated(-scroll));
        auto time = widget->findChild<QLabel *>();
        QVERIFY(time);
        QCOMPARE(time->palette().color(time->foregroundRole()), QColor(Qt::white));
    }

    void openerLifetime()
    {
        if (!qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK"))
            QSKIP("Enable QITE_TEST_PLAYBACK to test with a WebM/VP8 multimedia backend");
        class Opener : public ITEMediaOpener {
        public:
            int        opened = 0, closed = 0;
            QIODevice *open(QUrl &url) override
            {
                ++opened;
                QFile file(QStringLiteral(QITE_TEST_VIDEO));
                if (!file.open(QIODevice::ReadOnly))
                    return nullptr;
                auto buffer = new QBuffer;
                buffer->setData(file.readAll());
                buffer->open(QIODevice::ReadOnly);
                url = QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO));
                return buffer;
            }
            void close(QIODevice *device) override
            {
                ++closed;
                delete device;
            }
            QVariant metadata(const QUrl &) override { return {}; }
        } opener;
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl(QStringLiteral("share:test")), &opener);
        edit.show();
        QTRY_VERIFY(edit.findChild<QToolButton *>());
        QCOMPARE(opener.opened, 0);
        edit.findChild<QToolButton *>()->click();
        QTRY_COMPARE(opener.opened, 1);
        auto player = edit.findChild<QMediaPlayer *>();
        QVERIFY(player);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 0, 10000);
        auto fullscreenButton = edit.findChild<QToolButton *>(QStringLiteral("qite-video-fullscreen-button"));
        fullscreenButton->click();
        QVERIFY(fullscreenButton->window()->isFullScreen());
        QCOMPARE(fullscreenButton->window()->findChild<QMediaPlayer *>(), player);
        QCOMPARE(opener.opened, 1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QCOMPARE(player->playbackState(), QMediaPlayer::PlayingState);
#else
        QCOMPARE(player->state(), QMediaPlayer::PlayingState);
#endif
        fullscreenButton->click();
        QCOMPARE(opener.opened, 1);
        edit.clear();
        QTRY_COMPARE(opener.closed, 1);
        QVERIFY(edit.findChildren<QMediaPlayer *>().isEmpty());
    }

    void replayLooksUpCompletedLocalFile()
    {
        if (!qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK"))
            QSKIP("Enable QITE_TEST_PLAYBACK to test with a WebM/VP8 multimedia backend");
        class Opener : public ITEMediaOpener {
        public:
            int        opened = 0, closed = 0;
            QIODevice *open(QUrl &url) override
            {
                ++opened;
                url = QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO));
                if (opened > 1)
                    return nullptr;
                QFile file(QStringLiteral(QITE_TEST_VIDEO));
                if (!file.open(QIODevice::ReadOnly))
                    return nullptr;
                auto buffer = new QBuffer;
                buffer->setData(file.readAll());
                buffer->open(QIODevice::ReadOnly);
                return buffer;
            }
            void close(QIODevice *device) override
            {
                ++closed;
                delete device;
            }
            QVariant metadata(const QUrl &) override { return { }; }
        } opener;
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl(QStringLiteral("share:resource")), &opener);
        edit.show();
        QTRY_VERIFY(edit.findChild<QToolButton *>(QStringLiteral("qite-video-play")));
        auto button = edit.findChild<QToolButton *>(QStringLiteral("qite-video-play"));
        button->click();
        auto player = edit.findChild<QMediaPlayer *>();
        QVERIFY(player);
        QTRY_COMPARE_WITH_TIMEOUT(player->mediaStatus(), QMediaPlayer::EndOfMedia, 10000);
        button->click();
        QCOMPARE(opener.opened, 2);
        QCOMPARE(opener.closed, 1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QVERIFY(edit.findChild<QMediaPlayer *>()->sourceDevice() == nullptr);
        QVERIFY(edit.findChild<QMediaPlayer *>()->source().isLocalFile());
#endif
    }

    void sourceFailureRequiresExplicitRetry()
    {
        if (!qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK"))
            QSKIP("Enable QITE_TEST_PLAYBACK to test with a WebM/VP8 multimedia backend");
        class Opener : public StateOpener {
        public:
            int        retried = 0;
            QIODevice *open(QUrl &url) override
            {
                ++opens;
                url = QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO));
                return nullptr;
            }
            void retry(const QUrl &) override
            {
                ++retried;
                state.setSourceError({ });
            }
        } opener;
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl(QStringLiteral("share:resource")), &opener);
        edit.show();
        QTRY_VERIFY(edit.findChild<QToolButton *>(QStringLiteral("qite-video-play")));
        auto button = edit.findChild<QToolButton *>(QStringLiteral("qite-video-play"));
        button->click();
        auto player = edit.findChild<QMediaPlayer *>();
        QVERIFY(player);
        QTRY_VERIFY_WITH_TIMEOUT(player->position() > 0, 10000);
        opener.state.setSourceError(QStringLiteral("Checksum mismatch"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::StoppedState);
#else
        QTRY_COMPARE(player->state(), QMediaPlayer::StoppedState);
#endif
        QTest::qWait(30);
        QCOMPARE(opener.opens, 1);
        QCOMPARE(opener.retried, 0);
        QVERIFY(button->toolTip().contains(QStringLiteral("Checksum mismatch")));
        button->click();
        QCOMPARE(opener.retried, 1);
        QCOMPARE(opener.opens, 2);
    }

    void frameIsolation()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        edit.insertPlainText("Text above\n");
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        edit.insertPlainText("\nText below\n" + QString("Some chat text\n").repeated(2000));
        edit.moveCursor(QTextCursor::Start);
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        auto output = edit.findChild<ITEVideoFrameWidget *>();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QVERIFY(output->videoSurface()->start(QVideoSurfaceFormat(QSize(160, 90), QVideoFrame::Format_RGB32)));
#endif
        QTest::qWait(100);
        QSignalSpy changed(edit.document(), &QTextDocument::contentsChanged);
        QSignalSpy layout(edit.document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged);
        auto       makeFrame = [](int value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            QVideoFrame frame(QVideoFrameFormat(QSize(160, 90), QVideoFrameFormat::Format_BGRX8888));
            if (frame.map(QVideoFrame::WriteOnly)) {
                memset(frame.bits(0), value, size_t(frame.mappedBytes(0)));
#else
            QVideoFrame frame(160 * 90 * 4, QSize(160, 90), 160 * 4, QVideoFrame::Format_RGB32);
            if (frame.map(QAbstractVideoBuffer::WriteOnly)) {
                memset(frame.bits(), value, size_t(frame.mappedBytes()));
#endif
                frame.unmap();
            }
            return frame;
        };
        auto submit = [output](const QVideoFrame &frame) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            output->videoSink()->setVideoFrame(frame);
#else
            output->videoSurface()->present(frame);
#endif
        };
        submit(makeFrame(0));
        QTest::qWait(100); // Allow one-time output initialization/exposure.
        PaintCounter painted;
        output->installEventFilter(&painted);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QSignalSpy frames(output->videoSink(), &QVideoSink::videoFrameChanged);
#else
        QSignalSpy frames(output->videoSurface(), &ITEVideoSurface::frameReady);
#endif
        edit.paints = video.draws = 0;
        for (int i = 0; i < 120; ++i) {
            // Distinct frames: repeatedly setting the same QVideoFrame may be
            // elided by the sink and would not exercise the repaint path.
            submit(makeFrame(i + 1));
            QTest::qWait(2);
        }
        QCOMPARE(frames.count(), 120);
        QVERIFY(painted.paints > 0);
        const auto image = output->grab().toImage();
        QCOMPARE(image.pixelColor(image.rect().center()), QColor(120, 120, 120));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(layout.count(), 0);
        QCOMPARE(video.draws, 0);
        QCOMPARE(edit.paints, 0);
    }

    void geometryAndRemoval()
    {
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        edit.insertPlainText("Above\n");
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        edit.insertPlainText("\n" + QString("chat\n").repeated(200));
        edit.moveCursor(QTextCursor::Start);
        edit.show();
        QTRY_VERIFY(edit.findChild<ITEVideoFrameWidget *>());
        QPointer<QWidget> widget(edit.findChild<ITEVideoFrameWidget *>()->parentWidget());
        auto              cursor = text.findElement(1);
        QVERIFY(!cursor.isNull());
        QCOMPARE(widget->geometry(), text.elementRect(cursor));
        const auto initial = widget->geometry();
        edit.verticalScrollBar()->setValue(50);
        QTRY_COMPARE(widget->geometry(), initial.translated(0, -50));
        QVERIFY(widget->isVisible()); // Partially clipped by the viewport.
        QCOMPARE(widget->parentWidget(), edit.viewport());
        edit.verticalScrollBar()->setValue(500);
        QTRY_VERIFY(!widget->isVisible());
        edit.verticalScrollBar()->setValue(0);
        QTRY_VERIFY(widget->isVisible());
        QTextCursor top(edit.document());
        top.insertText("Inserted above\n");
        QTRY_VERIFY(widget->y() > initial.y());
        cursor = text.findElement(1);
        QTRY_COMPARE(widget->geometry(), text.elementRect(cursor));
        edit.resize(500, 500);
        QTRY_COMPARE(widget->geometry(), text.elementRect(cursor));
        cursor.removeSelectedText();
        QTRY_VERIFY(widget.isNull());
        QVERIFY(edit.findChildren<QMediaPlayer *>().isEmpty());
    }

    void delayedHttpPlayback()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        if (!qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK"))
            QSKIP("Enable QITE_TEST_PLAYBACK to test with a WebM/VP8 multimedia backend");
        QFile file(QStringLiteral(QITE_TEST_VIDEO));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket, bytes] {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("responded").toBool())
                    return;
                socket->setProperty("responded", true);
                // Longer than Qt/FFmpeg's default 20-second socket timeout.
                // Model an opener proxy waiting for its first upstream chunk.
                QTimer::singleShot(22000, socket, [socket, bytes] {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/webm\r\nContent-Length: "
                                  + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
                    socket->disconnectFromHost();
                });
            });
        });
        StateOpener     opener;
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl(QStringLiteral("http://127.0.0.1:%1/video.webm").arg(server.serverPort())), &opener);
        edit.show();
        QTRY_VERIFY(edit.findChild<QToolButton *>(QStringLiteral("qite-video-play")));
        edit.findChild<QToolButton *>(QStringLiteral("qite-video-play"))->click();
        auto player = edit.findChild<QMediaPlayer *>();
        QVERIFY(player);
        QSignalSpy frames(player->videoSink(), &QVideoSink::videoFrameChanged);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0 || player->error() != QMediaPlayer::NoError, 30000);
        QCOMPARE(player->error(), QMediaPlayer::NoError);
        QVERIFY(frames.count() > 0);
#else
        QSKIP("Configurable FFmpeg network timeout requires Qt 6.10");
#endif
    }

    void decoderPlayback()
    {
        if (!qEnvironmentVariableIntValue("QITE_TEST_PLAYBACK"))
            QSKIP("Enable QITE_TEST_PLAYBACK to test with a WebM/VP8 multimedia backend");
        TextEdit        edit;
        InteractiveText text(&edit);
        VideoController video(&text);
        video.insert(QUrl::fromLocalFile(QStringLiteral(QITE_TEST_VIDEO)));
        edit.insertPlainText("\n" + QString("chat\n").repeated(2000));
        edit.moveCursor(QTextCursor::Start);
        edit.show();
        QTRY_VERIFY(edit.findChild<QToolButton *>());
        QVERIFY(edit.findChildren<QMediaPlayer *>().isEmpty()); // No eager download/decoder.
        edit.findChild<QToolButton *>()->click();
        auto player = edit.findChild<QMediaPlayer *>();
        QVERIFY(player);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QSignalSpy frames(edit.findChild<ITEVideoFrameWidget *>()->videoSink(), &QVideoSink::videoFrameChanged);
#else
        QVideoProbe probe;
        QVERIFY(probe.setSource(player));
        QSignalSpy frames(&probe, &QVideoProbe::videoFrameProbed);
#endif
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= 5, 10000);
        QSignalSpy changed(edit.document(), &QTextDocument::contentsChanged);
        edit.paints = video.draws = 0;
        const int count           = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= count + 10, 5000);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(video.draws, 0);
        QCOMPARE(edit.paints, 0);
        auto button = edit.findChild<QToolButton *>();
        button->click(); // Pause before seeking, so the target stays stable.
        auto slider = edit.findChild<QSlider *>();
        QTRY_VERIFY(slider->isEnabled());
        slider->setValue(5000); // Also exercises keyboard/page-step seek handling.
        QTRY_VERIFY(qAbs(player->position() - player->duration() / 2) < 100);
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier,
                          QPoint(slider->width() * 3 / 4, slider->height() / 2));
        QTRY_VERIFY(qAbs(player->position() - player->duration() * 3 / 4) < 130);
        button->click();
        edit.hide();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
#else
        QTRY_COMPARE(player->state(), QMediaPlayer::PausedState);
#endif
        edit.show();
        button->click();
        edit.verticalScrollBar()->setValue(500);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QTRY_COMPARE(player->playbackState(), QMediaPlayer::PausedState);
#else
        QTRY_COMPARE(player->state(), QMediaPlayer::PausedState);
#endif
        edit.clear();
        QTRY_VERIFY(edit.findChildren<QMediaPlayer *>().isEmpty());
    }
};
QTEST_MAIN(VideoTest)
#include "video.moc"
