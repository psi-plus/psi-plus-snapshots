/* SPDX-License-Identifier: Apache-2.0 */
#include "qitevideo.h"
#include "qitevideoframe_p.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSlider>
#include <QSplitter>
#include <QTest>
#include <QTextTable>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVideoFrame>
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
        edit.clear(); // Deleting the message also closes its fullscreen window.
        QTRY_VERIFY(full.isNull());
        QVERIFY(edit.findChildren<ITEVideoFrameWidget *>().isEmpty());
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
        QCOMPARE(edit.findChild<QMediaPlayer *>(), player);
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
