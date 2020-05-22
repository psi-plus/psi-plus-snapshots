/*
 * Copyright (C) 2008-2009  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#ifndef RTPWORKER_H
#define RTPWORKER_H

#include "psimediaprovider.h"
#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QString>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace PsiMedia {

class PipelineDeviceContext;

class Stats;

// Note: do not destruct this class during one of its callbacks
class RtpWorker {
public:
    // this class exists in case we want to add metadata to the image,
    //   such as a timestamp
    class Frame {
    public:
        QImage image;

        static Frame pullFromSink(GstAppSink *appsink);
    };

    void *app = nullptr; // for callbacks

    QString             aout;
    QString             ain;
    QString             vin;
    QString             infile;
    QByteArray          indata;
    bool                loopFile = false;
    QList<PAudioParams> localAudioParams;
    QList<PVideoParams> localVideoParams;
    QList<PPayloadInfo> localAudioPayloadInfo;
    QList<PPayloadInfo> localVideoPayloadInfo;
    QList<PPayloadInfo> remoteAudioPayloadInfo;
    QList<PPayloadInfo> remoteVideoPayloadInfo;
    int                 maxbitrate = 0;

    // read-only
    bool canTransmitAudio;
    bool canTransmitVideo;
    int  outputVolume;
    int  inputVolume;
    int  error;

    explicit RtpWorker(GMainContext *mainContext);
    ~RtpWorker();

    RtpWorker(const RtpWorker &) = delete;
    RtpWorker &operator=(const RtpWorker &) = delete;

    void start();  // must wait until cb_updated before calling update
    void update(); // must wait until cb_updated before calling update
    void transmitAudio();
    void transmitVideo();
    void pauseAudio();
    void pauseVideo();
    void stop(); // can be called at any time after calling start

    // the rtp input functions are safe to call from any thread
    void rtpAudioIn(const PRtpPacket &packet);
    void rtpVideoIn(const PRtpPacket &packet);

    void setOutputVolume(int level);
    void setInputVolume(int level);

    void recordStart();
    void recordStop();

    // callbacks

    void (*cb_started)(void *app);
    void (*cb_updated)(void *app);
    void (*cb_stopped)(void *app);
    void (*cb_finished)(void *app);
    void (*cb_error)(void *app);
    void (*cb_audioOutputIntensity)(int value, void *app);
    void (*cb_audioInputIntensity)(int value, void *app);

    // callbacks - from alternate thread, be safe!
    //   also, it is not safe to assign callbacks except before starting

    void (*cb_previewFrame)(const Frame &frame, void *app);
    void (*cb_outputFrame)(const Frame &frame, void *app);
    void (*cb_rtpAudioOut)(const PRtpPacket &packet, void *app);
    void (*cb_rtpVideoOut)(const PRtpPacket &packet, void *app);

    // empty record packet = EOF/error
    void (*cb_recordData)(const QByteArray &packet, void *app);

private:
    GMainContext *mainContext_ = nullptr;
    GSource *     timer        = nullptr;

    PipelineDeviceContext *pd_audiosrc = nullptr, *pd_videosrc = nullptr, *pd_audiosink = nullptr;
    GstElement *           sendbin = nullptr, *recvbin = nullptr;

    GstElement *fileDemux   = nullptr;
    GstElement *audiosrc    = nullptr;
    GstElement *videosrc    = nullptr;
    GstElement *audiortpsrc = nullptr;
    GstElement *videortpsrc = nullptr;
    GstElement *audiortppay = nullptr;
    GstElement *videortppay = nullptr;
    GstElement *volumein    = nullptr;
    GstElement *volumeout   = nullptr;
    bool        rtpaudioout = false;
    bool        rtpvideoout = false;
    QMutex      audiortpsrc_mutex;
    QMutex      videortpsrc_mutex;
    QMutex      volumein_mutex;
    QMutex      volumeout_mutex;
    QMutex      rtpaudioout_mutex;
    QMutex      rtpvideoout_mutex;

    // GSource *recordTimer;

    QList<PPayloadInfo> actual_localAudioPayloadInfo;
    QList<PPayloadInfo> actual_localVideoPayloadInfo;
    QList<PPayloadInfo> actual_remoteAudioPayloadInfo;
    QList<PPayloadInfo> actual_remoteVideoPayloadInfo;

    Stats *audioStats = nullptr;
    Stats *videoStats = nullptr;

    void cleanup();

    static gboolean      cb_doStart(gpointer data);
    static gboolean      cb_doUpdate(gpointer data);
    static gboolean      cb_doStop(gpointer data);
    static void          cb_fileDemux_no_more_pads(GstElement *element, gpointer data);
    static void          cb_fileDemux_pad_added(GstElement *element, GstPad *pad, gpointer data);
    static void          cb_fileDemux_pad_removed(GstElement *element, GstPad *pad, gpointer data);
    static gboolean      cb_bus_call(GstBus *bus, GstMessage *msg, gpointer data);
    static GstFlowReturn cb_show_frame_preview(GstAppSink *appsink, gpointer data);
    static GstFlowReturn cb_show_frame_output(GstAppSink *appsink, gpointer data);
    static GstFlowReturn cb_packet_ready_rtp_audio(GstAppSink *appsink, gpointer data);
    static GstFlowReturn cb_packet_ready_rtp_video(GstAppSink *appsink, gpointer data);
    static GstFlowReturn cb_packet_ready_preroll_stub(GstAppSink *appsink, gpointer data);
    static void          cb_packet_ready_eos_stub(GstAppSink *appsink, gpointer data);
    static gboolean      cb_fileReady(gpointer data);

    gboolean      doStart();
    gboolean      doUpdate();
    gboolean      doStop();
    void          fileDemux_no_more_pads(GstElement *element);
    void          fileDemux_pad_added(GstElement *element, GstPad *pad);
    void          fileDemux_pad_removed(GstElement *element, GstPad *pad);
    gboolean      bus_call(GstBus *bus, GstMessage *msg);
    GstFlowReturn show_frame_preview(GstAppSink *appsink);
    GstFlowReturn show_frame_output(GstAppSink *appsink);
    GstFlowReturn packet_ready_rtp_audio(GstAppSink *appsink);
    GstFlowReturn packet_ready_rtp_video(GstAppSink *appsink);
    gboolean      fileReady();

    bool        setupSendRecv();
    bool        startSend();
    bool        startSend(int rate);
    bool        startRecv();
    bool        addAudioChain();
    bool        addAudioChain(int rate);
    bool        addVideoChain();
    bool        getCaps();
    bool        updateTheoraConfig();
    GstAppSink *makeVideoPlayAppSink(const gchar *name);
};

}

#endif
