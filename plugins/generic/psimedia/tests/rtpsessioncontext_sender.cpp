/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "gstprovider.h"
#include "gstrtpchannel.h"
#include "gstrtpsessioncontext.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gst/gst.h>

#include <memory>

namespace PsiMedia {

struct RtpSessionBridgeTestAccess {
    static bool postRuntimeError(RtpSessionBridge &bridge)
    {
        if (!bridge.bus_ || !bridge.pipeline_)
            return false;

        GError *error
            = g_error_new_literal(GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "synthetic RTP bridge runtime failure");
        GstMessage *message
            = gst_message_new_error(GST_OBJECT(bridge.pipeline_), error, "psimedia runtime-error regression");
        g_error_free(error);
        if (!message)
            return false;
        return gst_bus_post(bridge.bus_, message) != FALSE;
    }
};

} // namespace PsiMedia

namespace {

constexpr int     NegotiatedPayloadType = 109;
constexpr int     RawSampleRate         = 44100;
constexpr int     RawChannels           = 1;
constexpr int     OpusRtpClockRate      = 48000;
constexpr int     OpusRtpChannels       = 2;
constexpr quint32 DecodeRemoteSsrc      = 0x13572468;

bool isExpectedRtpPacket(const PsiMedia::PRtpPacket &packet)
{
    if (packet.type != PsiMedia::PRtpPacket::Type::Rtp || packet.rawValue.size() < 2)
        return false;

    const auto *bytes = reinterpret_cast<const uchar *>(packet.rawValue.constData());
    return (bytes[0] >> 6) == 2 && (bytes[1] & 0x7f) == NegotiatedPayloadType;
}

bool waitForPayloadPacket(PsiMedia::RtpChannelContext *channel, int payloadType,
                          PsiMedia::GstRtpSessionContext *session, int timeoutMs = 10000)
{
    bool       failed = false;
    const auto errorConnection
        = QObject::connect(session, &PsiMedia::GstRtpSessionContext::error, [&]() { failed = true; });

    QElapsedTimer timer;
    timer.start();
    while (!failed && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        while (channel->packetsAvailable() > 0) {
            const auto packet = channel->read();
            if (packet.type != PsiMedia::PRtpPacket::Type::Rtp || packet.rawValue.size() < 2)
                continue;
            const auto *bytes = reinterpret_cast<const uchar *>(packet.rawValue.constData());
            if ((bytes[0] >> 6) == 2 && (bytes[1] & 0x7f) == payloadType) {
                QObject::disconnect(errorConnection);
                return true;
            }
        }
        QThread::msleep(5);
    }

    QObject::disconnect(errorConnection);
    return false;
}

QList<PsiMedia::PRtpPacket> waitForRtpPackets(PsiMedia::GstRtpChannel        *audioChannel,
                                              PsiMedia::GstRtpSessionContext *session, int count = 6,
                                              int timeoutMs = 10000)
{
    QList<PsiMedia::PRtpPacket> result;
    bool                        failed = false;
    const auto                  errorConnection
        = QObject::connect(session, &PsiMedia::GstRtpSessionContext::error, [&]() { failed = true; });

    QElapsedTimer timer;
    timer.start();
    while (!failed && result.size() < count && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        while (audioChannel->packetsAvailable() > 0 && result.size() < count) {
            const auto packet = audioChannel->read();
            if (isExpectedRtpPacket(packet))
                result.append(packet);
        }
        if (result.size() < count)
            QThread::msleep(5);
    }

    QObject::disconnect(errorConnection);
    return result;
}

quint64 remoteRtpPacketsProcessed(PsiMedia::RtpSessionBridge &bridge, quint32 ssrc)
{
    GstStructure *stats = bridge.sessionStats();
    if (!stats)
        return 0;

    guint64       packets      = 0;
    const GValue *sourcesValue = gst_structure_get_value(stats, "source-stats");
    if (sourcesValue) {
        auto *sources = static_cast<GValueArray *>(g_value_get_boxed(sourcesValue));
        if (sources) {
            for (guint i = 0; i < sources->n_values; ++i) {
                const auto *source     = static_cast<const GstStructure *>(g_value_get_boxed(&sources->values[i]));
                guint       sourceSsrc = 0;
                if (source && gst_structure_get_uint(source, "ssrc", &sourceSsrc) && sourceSsrc == ssrc) {
                    gst_structure_get_uint64(source, "packets-received", &packets);
                    break;
                }
            }
        }
    }
    gst_structure_free(stats);
    return packets;
}

void makeRemoteRtp(PsiMedia::PRtpPacket &packet, quint16 sequence, quint32 timestamp)
{
    if (packet.rawValue.size() < 12)
        return;

    auto *bytes = reinterpret_cast<uchar *>(packet.rawValue.data());
    bytes[2]    = uchar(sequence >> 8);
    bytes[3]    = uchar(sequence);
    bytes[4]    = uchar(timestamp >> 24);
    bytes[5]    = uchar(timestamp >> 16);
    bytes[6]    = uchar(timestamp >> 8);
    bytes[7]    = uchar(timestamp);

    bytes[8]  = uchar(DecodeRemoteSsrc >> 24);
    bytes[9]  = uchar(DecodeRemoteSsrc >> 16);
    bytes[10] = uchar(DecodeRemoteSsrc >> 8);
    bytes[11] = uchar(DecodeRemoteSsrc);
}

bool waitForDecodedOutput(PsiMedia::GstRtpChannel *audioChannel, PsiMedia::RtpSessionBridge &bridge,
                          const QList<PsiMedia::PRtpPacket> &packets, const QString &outputPath, quint16 &sequence,
                          quint32 &timestamp, int timeoutMs = 5000)
{
    const qint64  initialSize      = QFileInfo(outputPath).exists() ? QFileInfo(outputPath).size() : 0;
    const quint64 initialProcessed = remoteRtpPacketsProcessed(bridge, DecodeRemoteSsrc);
    int           written          = 0;
    for (auto packet : packets) {
        if (packet.rawValue.size() < 12)
            continue;
        makeRemoteRtp(packet, sequence++, timestamp);
        timestamp += 960; // 20 ms at the negotiated 48 kHz Opus RTP clock.
        audioChannel->write(packet);
        ++written;
    }
    if (!written)
        return false;

    QElapsedTimer bridgeTimer;
    bridgeTimer.start();
    while (bridgeTimer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (remoteRtpPacketsProcessed(bridge, DecodeRemoteSsrc) > initialProcessed)
            break;
        QThread::msleep(5);
    }
    const quint64 processed = remoteRtpPacketsProcessed(bridge, DecodeRemoteSsrc);
    if (processed <= initialProcessed) {
        qCritical() << "RTP bridge did not validate reflected remote packets" << initialProcessed << processed
                    << written;
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (QFileInfo(outputPath).exists() && QFileInfo(outputPath).size() > initialSize)
            return true;
        QThread::msleep(5);
    }
    return false;
}

bool waitForControlBarrier(PsiMedia::RtpSessionContext *session, int timeoutMs = 5000)
{
    bool       done = false;
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    session->dumpPipeline([&](const QStringList &) {
        QMetaObject::invokeMethod(
            &loop,
            [&]() {
                done = true;
                loop.quit();
            },
            Qt::QueuedConnection);
    });

    timer.start(timeoutMs);
    loop.exec();
    return done;
}

QString receiveAppSrcName(PsiMedia::RtpSessionContext *session, const QString &media = QStringLiteral("audio"),
                          int timeoutMs = 5000)
{
    QString    dotPath;
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    session->dumpPipeline([&](const QStringList &paths) {
        QString recvPath;
        for (const auto &path : paths) {
            if (path.endsWith(QStringLiteral("psimedia_recv.dot"))) {
                recvPath = path;
                break;
            }
        }
        QMetaObject::invokeMethod(
            &loop,
            [&, recvPath]() {
                dotPath = recvPath;
                loop.quit();
            },
            Qt::QueuedConnection);
    });

    timer.start(timeoutMs);
    loop.exec();
    if (dotPath.isEmpty())
        return {};

    QFile file(dotPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString dot     = QString::fromUtf8(file.readAll());
    const auto    pattern = QStringLiteral("psimedia_%1_rtp_recv_\\d+").arg(media);
    const auto    match   = QRegularExpression(pattern).match(dot);
    return match.hasMatch() ? match.captured(0) : QString();
}

void drainPackets(PsiMedia::GstRtpChannel *channel)
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    while (channel->packetsAvailable() > 0)
        channel->read();
}

bool waitForRtpQuiet(PsiMedia::GstRtpChannel *audioChannel, PsiMedia::GstRtpSessionContext *session, int quietMs = 500,
                     int timeoutMs = 10000)
{
    bool       failed = false;
    const auto errorConnection
        = QObject::connect(session, &PsiMedia::GstRtpSessionContext::error, [&]() { failed = true; });

    QElapsedTimer total;
    QElapsedTimer quiet;
    total.start();
    quiet.start();

    while (total.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (failed) {
            QObject::disconnect(errorConnection);
            return false;
        }

        bool sawRtp = false;
        while (audioChannel->packetsAvailable() > 0) {
            const auto packet = audioChannel->read();
            if (isExpectedRtpPacket(packet))
                sawRtp = true;
        }
        if (sawRtp)
            quiet.restart();
        if (quiet.elapsed() >= quietMs) {
            QObject::disconnect(errorConnection);
            return true;
        }
        QThread::msleep(5);
    }

    QObject::disconnect(errorConnection);
    return false;
}

bool createFiniteOpusFile(const QString &path)
{
    GstElement *pipeline = gst_pipeline_new("psimedia-test-file");
    GstElement *source   = gst_element_factory_make("audiotestsrc", nullptr);
    GstElement *convert  = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);
    GstElement *caps     = gst_element_factory_make("capsfilter", nullptr);
    GstElement *encoder  = gst_element_factory_make("opusenc", nullptr);
    GstElement *mux      = gst_element_factory_make("oggmux", nullptr);
    GstElement *sink     = gst_element_factory_make("filesink", nullptr);
    if (!pipeline || !source || !convert || !resample || !caps || !encoder || !mux || !sink) {
        if (pipeline)
            gst_object_unref(pipeline);
        return false;
    }

    g_object_set(source, "is-live", FALSE, "num-buffers", 50, nullptr);
    g_object_set(sink, "location", QFile::encodeName(path).constData(), nullptr);
    GstCaps *rawCaps = gst_caps_new_simple("audio/x-raw", "rate", G_TYPE_INT, OpusRtpClockRate, "channels", G_TYPE_INT,
                                           OpusRtpChannels, nullptr);
    g_object_set(caps, "caps", rawCaps, nullptr);
    gst_caps_unref(rawCaps);

    gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, caps, encoder, mux, sink, nullptr);
    if (!gst_element_link_many(source, convert, resample, caps, encoder, mux, sink, nullptr)) {
        gst_object_unref(pipeline);
        return false;
    }

    const auto stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }

    GstBus     *bus = gst_element_get_bus(pipeline);
    GstMessage *msg
        = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg)
        gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok && QFileInfo(path).size() > 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // GStreamer reads GST_DEBUG_DUMP_DOT_DIR during initialization on older
    // 1.24.x builds. Set it before GstProvider triggers gst_init().
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        qCritical() << "Could not create temporary test directory";
        return 1;
    }
    qputenv("GST_DEBUG_DUMP_DOT_DIR", QFile::encodeName(tempDir.path()));

    PsiMedia::GstProvider provider;
    if (!provider.isInitialized()) {
        qCritical() << "GStreamer provider failed to initialize";
        return 1;
    }

    std::unique_ptr<PsiMedia::RtpSessionContext> session(provider.createRtpSession());
    auto *gstSession   = qobject_cast<PsiMedia::GstRtpSessionContext *>(session->qobject());
    auto *audioChannel = qobject_cast<PsiMedia::GstRtpChannel *>(session->audioRtpChannel()->qobject());
    if (!gstSession || !audioChannel) {
        qCritical() << "Provider did not create the production GStreamer RTP session";
        return 2;
    }

    PsiMedia::PAudioParams rawAudio;
    rawAudio.codec      = QStringLiteral("opus");
    rawAudio.sampleRate = RawSampleRate;
    rawAudio.sampleSize = 16;
    rawAudio.channels   = RawChannels;
    session->setLocalAudioPreferences({ rawAudio });

    PsiMedia::PPayloadInfo remoteOpus;
    remoteOpus.id        = NegotiatedPayloadType;
    remoteOpus.name      = QStringLiteral("OPUS");
    remoteOpus.clockrate = OpusRtpClockRate;
    remoteOpus.channels  = OpusRtpChannels;
    session->setRemoteAudioPreferences({ remoteOpus });

    // Use the normal AudioOut device path but write decoded PCM to a file so
    // the regression can prove receive depay/decode/output processing without
    // requiring a physical audio device.
    const QString decodedOutputPath = tempDir.filePath(QStringLiteral("decoded.raw"));
    session->setAudioOutputDevice(
        QStringLiteral("filesink location=\"%1\" buffer-mode=unbuffered sync=false async=false")
            .arg(decodedOutputPath));

    // Negotiation must not require capture. The live source is attached only
    // after the session has started, matching Psi's consent/no-microphone path.
    audioChannel->setEnabled(true);

    bool       startFailed   = false;
    bool       startTimedOut = false;
    QEventLoop startLoop;
    QTimer     startTimer;
    startTimer.setSingleShot(true);
    QObject::connect(&startTimer, &QTimer::timeout, &startLoop, [&]() {
        startTimedOut = true;
        startLoop.quit();
    });
    QObject::connect(gstSession, &PsiMedia::GstRtpSessionContext::started, &startLoop, &QEventLoop::quit);
    QObject::connect(gstSession, &PsiMedia::GstRtpSessionContext::error, &startLoop, [&]() {
        startFailed = true;
        startLoop.quit();
    });

    startTimer.start(10000);
    session->start();
    startLoop.exec();
    startTimer.stop();

    if (startFailed || startTimedOut) {
        qCritical() << (startFailed ? "Production RTP session failed to start"
                                    : "Timed out starting production RTP session");
        return 3;
    }

    const auto localPayloads = session->localAudioPayloadInfo();
    if (localPayloads.size() != 1) {
        qCritical() << "Device-independent negotiation did not expose one Opus payload";
        return 4;
    }

    const auto &localOpus = localPayloads.constFirst();
    if (localOpus.id != NegotiatedPayloadType
        || localOpus.name.compare(QStringLiteral("OPUS"), Qt::CaseInsensitive) != 0
        || localOpus.clockrate != OpusRtpClockRate || localOpus.channels != OpusRtpChannels) {
        qCritical() << "Unexpected negotiated Opus payload" << localOpus.id << localOpus.name << localOpus.clockrate
                    << localOpus.channels;
        return 5;
    }

    const QString filePath = tempDir.filePath(QStringLiteral("switch.ogg"));
    if (!createFiniteOpusFile(filePath)) {
        qCritical() << "Could not create finite Ogg/Opus test input";
        return 6;
    }

    const QString receiveBeforeSwitch = receiveAppSrcName(session.get());
    if (receiveBeforeSwitch.isEmpty()) {
        qCritical() << "Could not identify the production receive appsrc";
        return 7;
    }

    // Start with an unbounded live source. A live->file switch must tear this
    // capture source down before the finite file source is committed.
    session->setAudioInputDevice(QStringLiteral("audiotestsrc is-live=true wave=sine"));
    if (!waitForControlBarrier(session.get())) {
        qCritical() << "Timed out attaching the live synthetic audio input";
        return 7;
    }
    session->transmitAudio();
    const auto livePackets = waitForRtpPackets(audioChannel, gstSession);
    if (livePackets.size() < 6) {
        qCritical() << "Timed out waiting for RTP from the live input";
        return 8;
    }

    quint16 remoteSequence  = 1;
    quint32 remoteTimestamp = 48000;
    if (!waitForDecodedOutput(audioChannel, gstSession->audioBridge, livePackets, decodedOutputPath, remoteSequence,
                              remoteTimestamp)) {
        qCritical() << "Production receive path did not decode live-source RTP";
        return 8;
    }

    drainPackets(audioChannel);
    session->setFileInput(filePath);
    if (!waitForControlBarrier(session.get())) {
        qCritical() << "Timed out switching from live input to file input";
        return 9;
    }
    const QString receiveAfterFileSwitch = receiveAppSrcName(session.get());
    if (receiveAfterFileSwitch != receiveBeforeSwitch) {
        qCritical() << "Live-to-file capture switch recreated the receive pipeline" << receiveBeforeSwitch
                    << receiveAfterFileSwitch;
        return 10;
    }
    // Preserve the previous transmit intent across the source replacement.
    session->transmitAudio();
    const auto filePackets = waitForRtpPackets(audioChannel, gstSession);
    if (filePackets.size() < 6) {
        qCritical() << "File input did not produce RTP after replacing live capture";
        return 10;
    }
    if (!waitForDecodedOutput(audioChannel, gstSession->audioBridge, filePackets, decodedOutputPath, remoteSequence,
                              remoteTimestamp)) {
        qCritical() << "Receive decode/output stopped after live-to-file capture switch";
        return 10;
    }
    // A finite file must eventually stop producing RTP. The previous live
    // audiotestsrc is deliberately unbounded, so continued capture cannot
    // satisfy this quiet-window oracle even though the legacy provider does
    // not surface appsink EOS as RtpSessionContext::finished().
    if (!waitForRtpQuiet(audioChannel, gstSession)) {
        qCritical() << "RTP did not quiesce after finite file input";
        return 11;
    }

    // The session/bridge survives source replacement. Switching back to a
    // different live source must rebuild capture and resume RTP without
    // recreating GstRtpSessionContext.
    session->setAudioInputDevice(QStringLiteral("audiotestsrc is-live=true wave=white-noise"));
    if (!waitForControlBarrier(session.get())) {
        qCritical() << "Timed out switching from file input back to live input";
        return 12;
    }
    const QString receiveAfterLiveSwitch = receiveAppSrcName(session.get());
    if (receiveAfterLiveSwitch != receiveBeforeSwitch) {
        qCritical() << "File-to-live capture switch recreated the receive pipeline" << receiveBeforeSwitch
                    << receiveAfterLiveSwitch;
        return 13;
    }
    session->transmitAudio();
    const auto resumedLivePackets = waitForRtpPackets(audioChannel, gstSession);
    if (resumedLivePackets.size() < 6) {
        qCritical() << "RTP did not resume after file-to-live switch";
        return 13;
    }
    if (!waitForDecodedOutput(audioChannel, gstSession->audioBridge, resumedLivePackets, decodedOutputPath,
                              remoteSequence, remoteTimestamp)) {
        qCritical() << "Receive decode/output stopped after file-to-live capture switch";
        return 13;
    }

    session->pauseAudio();
    session->setAudioInputDevice(QString());
    if (!waitForControlBarrier(session.get())) {
        qCritical() << "Timed out detaching the final synthetic audio input";
        return 14;
    }
    drainPackets(audioChannel);

    bool       stopTimedOut = false;
    QEventLoop stopLoop;
    QTimer     stopTimer;
    stopTimer.setSingleShot(true);
    QObject::connect(&stopTimer, &QTimer::timeout, &stopLoop, [&]() {
        stopTimedOut = true;
        stopLoop.quit();
    });
    QObject::connect(gstSession, &PsiMedia::GstRtpSessionContext::stopped, &stopLoop, &QEventLoop::quit);

    stopTimer.start(10000);
    session->stop();
    stopLoop.exec();
    stopTimer.stop();

    if (stopTimedOut) {
        qCritical() << "Timed out stopping production RTP session";
        return 15;
    }

    // Reproduce the Jingle adapter ordering: audio starts the receive graph,
    // then video is negotiated on the already-running session. The regression
    // checks the topology invariant directly; end-to-end VP8 decoding is covered
    // by the live Prosody A/V BUNDLE integration job.
    std::unique_ptr<PsiMedia::RtpSessionContext> videoSession(provider.createRtpSession());
    auto *videoGstSession = qobject_cast<PsiMedia::GstRtpSessionContext *>(videoSession->qobject());
    if (!videoGstSession) {
        qCritical() << "Provider did not create the late-video test session";
        return 16;
    }

    videoSession->setLocalAudioPreferences({ rawAudio });
    videoSession->setRemoteAudioPreferences({ remoteOpus });
    videoSession->audioRtpChannel()->setEnabled(true);

    bool       videoStartFailed   = false;
    bool       videoStartTimedOut = false;
    QEventLoop videoStartLoop;
    QTimer     videoStartTimer;
    videoStartTimer.setSingleShot(true);
    QObject::connect(&videoStartTimer, &QTimer::timeout, &videoStartLoop, [&]() {
        videoStartTimedOut = true;
        videoStartLoop.quit();
    });
    QObject::connect(videoGstSession, &PsiMedia::GstRtpSessionContext::started, &videoStartLoop, &QEventLoop::quit);
    QObject::connect(videoGstSession, &PsiMedia::GstRtpSessionContext::error, &videoStartLoop, [&]() {
        videoStartFailed = true;
        videoStartLoop.quit();
    });

    videoStartTimer.start(10000);
    videoSession->start();
    videoStartLoop.exec();
    videoStartTimer.stop();
    if (videoStartFailed || videoStartTimedOut) {
        qCritical() << "Audio-first late-video test session did not start";
        return 16;
    }

    const QString audioReceiveBeforeVideo = receiveAppSrcName(videoSession.get());
    if (audioReceiveBeforeVideo.isEmpty()) {
        qCritical() << "Audio-first session did not create its receive appsrc";
        return 16;
    }
    if (!receiveAppSrcName(videoSession.get(), QStringLiteral("video")).isEmpty()) {
        qCritical() << "Video receive appsrc existed before video negotiation";
        return 16;
    }

    PsiMedia::PVideoParams localVideo;
    localVideo.codec = QStringLiteral("vp8");
    localVideo.size  = QSize(320, 240);
    localVideo.fps   = 15;
    videoSession->setLocalVideoPreferences({ localVideo });

    PsiMedia::PPayloadInfo remoteVp8;
    remoteVp8.id        = 96;
    remoteVp8.name      = QStringLiteral("VP8");
    remoteVp8.clockrate = 90000;
    videoSession->setRemoteVideoPreferences({ remoteVp8 });

    bool       videoUpdateFailed   = false;
    bool       videoUpdateTimedOut = false;
    QEventLoop videoUpdateLoop;
    QTimer     videoUpdateTimer;
    videoUpdateTimer.setSingleShot(true);
    QObject::connect(&videoUpdateTimer, &QTimer::timeout, &videoUpdateLoop, [&]() {
        videoUpdateTimedOut = true;
        videoUpdateLoop.quit();
    });
    QObject::connect(videoGstSession, &PsiMedia::GstRtpSessionContext::preferencesUpdated, &videoUpdateLoop,
                     &QEventLoop::quit);
    QObject::connect(videoGstSession, &PsiMedia::GstRtpSessionContext::error, &videoUpdateLoop, [&]() {
        videoUpdateFailed = true;
        videoUpdateLoop.quit();
    });

    videoUpdateTimer.start(10000);
    videoSession->updatePreferences();
    videoUpdateLoop.exec();
    videoUpdateTimer.stop();
    if (videoUpdateFailed || videoUpdateTimedOut) {
        qCritical() << "Adding video to the running audio receive graph failed";
        return 16;
    }

    const QString audioReceiveAfterVideo = receiveAppSrcName(videoSession.get());
    const QString videoReceiveAfterVideo = receiveAppSrcName(videoSession.get(), QStringLiteral("video"));
    if (audioReceiveAfterVideo != audioReceiveBeforeVideo) {
        qCritical() << "Late video negotiation rebuilt the live audio receive graph" << audioReceiveBeforeVideo
                    << audioReceiveAfterVideo;
        return 16;
    }
    if (videoReceiveAfterVideo.isEmpty()) {
        qCritical() << "Late video negotiation did not add the video receive appsrc";
        return 16;
    }

    const auto negotiatedVideo = videoSession->remoteVideoPayloadInfo();
    if (negotiatedVideo.size() != 1 || negotiatedVideo.constFirst().id != remoteVp8.id
        || negotiatedVideo.constFirst().name.compare(QStringLiteral("VP8"), Qt::CaseInsensitive) != 0
        || negotiatedVideo.constFirst().clockrate != remoteVp8.clockrate) {
        qCritical() << "Late video negotiation did not commit VP8 receive status";
        return 16;
    }

    bool       videoStopTimedOut = false;
    QEventLoop videoStopLoop;
    QTimer     videoStopTimer;
    videoStopTimer.setSingleShot(true);
    QObject::connect(&videoStopTimer, &QTimer::timeout, &videoStopLoop, [&]() {
        videoStopTimedOut = true;
        videoStopLoop.quit();
    });
    QObject::connect(videoGstSession, &PsiMedia::GstRtpSessionContext::stopped, &videoStopLoop, &QEventLoop::quit);
    videoStopTimer.start(10000);
    videoSession->stop();
    videoStopLoop.exec();
    videoStopTimer.stop();
    if (videoStopTimedOut) {
        qCritical() << "Timed out stopping late-video test session";
        return 16;
    }
    videoSession.reset();

    // A bridge can fail after PLAYING even when the legacy media worker is
    // otherwise healthy. Verify that a real GstBus error reaches the provider
    // terminal error path exactly once on the Qt owner thread.
    std::unique_ptr<PsiMedia::RtpSessionContext> errorSession(provider.createRtpSession());
    auto *errorGstSession = qobject_cast<PsiMedia::GstRtpSessionContext *>(errorSession->qobject());
    if (!errorGstSession) {
        qCritical() << "Provider did not create the bridge-error test session";
        return 16;
    }

    errorSession->setLocalAudioPreferences({ rawAudio });
    errorSession->setRemoteAudioPreferences({ remoteOpus });
    errorSession->audioRtpChannel()->setEnabled(true);

    bool       errorStartFailed   = false;
    bool       errorStartTimedOut = false;
    QEventLoop errorStartLoop;
    QTimer     errorStartTimer;
    errorStartTimer.setSingleShot(true);
    QObject::connect(&errorStartTimer, &QTimer::timeout, &errorStartLoop, [&]() {
        errorStartTimedOut = true;
        errorStartLoop.quit();
    });
    QObject::connect(errorGstSession, &PsiMedia::GstRtpSessionContext::started, &errorStartLoop, &QEventLoop::quit);
    QObject::connect(errorGstSession, &PsiMedia::GstRtpSessionContext::error, &errorStartLoop, [&]() {
        errorStartFailed = true;
        errorStartLoop.quit();
    });

    errorStartTimer.start(10000);
    errorSession->start();
    errorStartLoop.exec();
    errorStartTimer.stop();
    if (errorStartFailed || errorStartTimedOut) {
        qCritical() << "Bridge-error test session did not start";
        return 17;
    }

    int        runtimeErrors    = 0;
    bool       wrongErrorThread = false;
    QThread   *ownerThread      = QThread::currentThread();
    QEventLoop runtimeErrorLoop;
    QTimer     runtimeErrorTimer;
    runtimeErrorTimer.setSingleShot(true);
    QObject::connect(&runtimeErrorTimer, &QTimer::timeout, &runtimeErrorLoop, &QEventLoop::quit);
    QObject::connect(errorGstSession, &PsiMedia::GstRtpSessionContext::error, &runtimeErrorLoop, [&]() {
        ++runtimeErrors;
        wrongErrorThread |= QThread::currentThread() != ownerThread;
        runtimeErrorLoop.quit();
    });

    if (!PsiMedia::RtpSessionBridgeTestAccess::postRuntimeError(errorGstSession->audioBridge)) {
        qCritical() << "Could not post synthetic RTP bridge bus error";
        return 18;
    }

    runtimeErrorTimer.start(3000);
    runtimeErrorLoop.exec();
    runtimeErrorTimer.stop();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (runtimeErrors != 1 || wrongErrorThread
        || errorGstSession->errorCode() != PsiMedia::RtpSessionContext::ErrorGeneric) {
        qCritical() << "RTP bridge runtime error did not reach provider terminal path" << runtimeErrors
                    << wrongErrorThread << int(errorGstSession->errorCode());
        return 19;
    }

    qInfo() << "Production RTP sender hotplug and bridge runtime-error regressions passed with Opus RTP PT"
            << NegotiatedPayloadType;
    return 0;
}
