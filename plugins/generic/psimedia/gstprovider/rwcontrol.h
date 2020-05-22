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

#ifndef RWCONTROL_H
#define RWCONTROL_H

#include "psimediaprovider.h"
#include "rtpworker.h"
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWaitCondition>
#include <glib.h>

namespace PsiMedia {

// These classes allow controlling RtpWorker from across the qt<->glib
// thread boundary.
//
// RwControlLocal  - object to live in "local" Qt eventloop
// RwControlRemote - object to live in "remote" glib eventloop
//
// When RwControlLocal is created, you pass it the GstMainLoop.  The constructor
// atomically creates a corresponding RwControlRemote in the remote thread and
// associates the two objects.
//
// The possible exchanges are made clear here.  Things you can do:
//
// - Start a session.  This requires device and codec configuration to begin.
//   This operation is a transaction, you'll receive a status message when it
//   completes.
//
// - Stop a session.  This operation is a transaction, you'll receive a
//   status message when it completes.
//
// - Update complete device configuration.  This is fire and forget.
//   Eventually it will take effect, and you won't be notified when it
//   happens.  From a local standpoint you simply assume it took effect
//   immediately.
//
// - Update codec configuration.  This is a transaction, you'll receive a
//   status message when it completes.
//
// - Transmit/pause the audio/video streams.  This is fire and forget.
//
// - Start/stop recording a session.  For starting, this is somewhat fire
//   and forget.  You'll eventually start receiving data packets, but the
//   assumption is that recording is occurring even before the first packet
//   is received.  For stopping, this is somewhat transactional.  The record
//   is not considered stopped until an EOF packet is received.
//
// - At any time, it is possible to receive a spontaneous status message.
//   This is to indicate an error or a completed file playback.
//
// - Preview and output video frames are signaled normally and are intended
//   for immediate display.
//
// - RTP packets and recording data bypass the event-based message-passing
//   mechanisms described above.  Instead, special methods and callbacks are
//   used which require special care.

class GstMainLoop;
class RwControlRemote;

class RwControlConfigDevices {
public:
    QString    audioOutId;
    QString    audioInId;
    QString    videoInId;
    QString    fileNameIn;
    QByteArray fileDataIn;
    bool       loopFile;
    bool       useVideoPreview;
    bool       useVideoOut;
    int        audioOutVolume;
    int        audioInVolume;

    RwControlConfigDevices() :
        loopFile(false), useVideoPreview(false), useVideoOut(false), audioOutVolume(-1), audioInVolume(-1)
    {
    }
};

class RwControlConfigCodecs {
public:
    bool useLocalAudioParams;
    bool useLocalVideoParams;
    bool useRemoteAudioPayloadInfo;
    bool useRemoteVideoPayloadInfo;

    QList<PAudioParams> localAudioParams;
    QList<PVideoParams> localVideoParams;
    QList<PPayloadInfo> remoteAudioPayloadInfo;
    QList<PPayloadInfo> remoteVideoPayloadInfo;

    int maximumSendingBitrate;

    RwControlConfigCodecs() :
        useLocalAudioParams(false), useLocalVideoParams(false), useRemoteAudioPayloadInfo(false),
        useRemoteVideoPayloadInfo(false), maximumSendingBitrate(-1)
    {
    }
};

class RwControlTransmit {
public:
    bool useAudio;
    bool useVideo;

    RwControlTransmit() : useAudio(false), useVideo(false) { }
};

class RwControlRecord {
public:
    bool enabled;

    RwControlRecord() : enabled(false) { }
};

// note: if this is received spontaneously, then only finished, error, and
//   errorCode are valid
class RwControlStatus {
public:
    QList<PAudioParams> localAudioParams;
    QList<PVideoParams> localVideoParams;
    QList<PPayloadInfo> localAudioPayloadInfo;
    QList<PPayloadInfo> localVideoPayloadInfo;
    QList<PPayloadInfo> remoteAudioPayloadInfo;
    QList<PPayloadInfo> remoteVideoPayloadInfo;
    bool                canTransmitAudio;
    bool                canTransmitVideo;

    bool stopped;
    bool finished;
    bool error;
    int  errorCode;

    RwControlStatus() :
        canTransmitAudio(false), canTransmitVideo(false), stopped(false), finished(false), error(false), errorCode(-1)
    {
    }
};

class RwControlAudioIntensity {
public:
    enum Type { Output, Input };

    Type type;
    int  value;

    RwControlAudioIntensity() : type((Type)-1), value(-1) { }
};

// always remote -> local, for internal use
class RwControlFrame {
public:
    enum Type { Preview, Output };

    Type   type;
    QImage image;
};

// internal
class RwControlMessage {
public:
    enum Type { Start, Stop, UpdateDevices, UpdateCodecs, Transmit, Record, Status, AudioIntensity, Frame };

    Type type;

    explicit RwControlMessage(Type _type) : type(_type) { }

    virtual ~RwControlMessage() = default;
};

class RwControlStartMessage : public RwControlMessage {
public:
    RwControlConfigDevices devices;
    RwControlConfigCodecs  codecs;

    RwControlStartMessage() : RwControlMessage(RwControlMessage::Start) { }
};

class RwControlStopMessage : public RwControlMessage {
public:
    RwControlStopMessage() : RwControlMessage(RwControlMessage::Stop) { }
};

class RwControlUpdateDevicesMessage : public RwControlMessage {
public:
    RwControlConfigDevices devices;

    RwControlUpdateDevicesMessage() : RwControlMessage(RwControlMessage::UpdateDevices) { }
};

class RwControlUpdateCodecsMessage : public RwControlMessage {
public:
    RwControlConfigCodecs codecs;

    RwControlUpdateCodecsMessage() : RwControlMessage(RwControlMessage::UpdateCodecs) { }
};

class RwControlTransmitMessage : public RwControlMessage {
public:
    RwControlTransmit transmit;

    RwControlTransmitMessage() : RwControlMessage(RwControlMessage::Transmit) { }
};

class RwControlRecordMessage : public RwControlMessage {
public:
    RwControlRecord record;

    RwControlRecordMessage() : RwControlMessage(RwControlMessage::Record) { }
};

class RwControlStatusMessage : public RwControlMessage {
public:
    RwControlStatus status;

    RwControlStatusMessage() : RwControlMessage(RwControlMessage::Status) { }
};

class RwControlAudioIntensityMessage : public RwControlMessage {
public:
    RwControlAudioIntensity intensity;

    RwControlAudioIntensityMessage() : RwControlMessage(RwControlMessage::AudioIntensity) { }
};

class RwControlFrameMessage : public RwControlMessage {
public:
    RwControlFrame frame;

    RwControlFrameMessage() : RwControlMessage(RwControlMessage::Frame), frame() { }
};

class RwControlLocal : public QObject {
    Q_OBJECT

public:
    explicit RwControlLocal(GstMainLoop *thread, QObject *parent = nullptr);
    ~RwControlLocal() override;

    void start(const RwControlConfigDevices &devices, const RwControlConfigCodecs &codecs);
    void stop(); // if called, may still receive many status messages before stopped
    void updateDevices(const RwControlConfigDevices &devices);
    void updateCodecs(const RwControlConfigCodecs &codecs);
    void setTransmit(const RwControlTransmit &transmit);
    void setRecord(const RwControlRecord &record);

    // can be called from any thread
    void rtpAudioIn(const PRtpPacket &packet);
    void rtpVideoIn(const PRtpPacket &packet);

    // can come from any thread.
    // note that it is only safe to assign callbacks prior to starting.
    // note if the stream is stopped while recording is active, then
    //   stopped status will not be reported until EOF is delivered.
    void *app;
    void (*cb_rtpAudioOut)(const PRtpPacket &packet, void *app);
    void (*cb_rtpVideoOut)(const PRtpPacket &packet, void *app);
    void (*cb_recordData)(const QByteArray &packet, void *app);

signals:
    // response to start, stop, updateCodecs, or it could be spontaneous
    void statusReady(const RwControlStatus &status);

    void previewFrame(const QImage &img);
    void outputFrame(const QImage &img);
    void audioOutputIntensityChanged(int intensity);
    void audioInputIntensityChanged(int intensity);

private slots:
    void processMessages();

private:
    GstMainLoop *    thread_;
    GSource *        timer;
    QMutex           m;
    QWaitCondition   w;
    RwControlRemote *remote_;
    bool             wake_pending;

    QMutex                    in_mutex;
    QList<RwControlMessage *> in;

    static gboolean cb_doCreateRemote(gpointer data);
    static gboolean cb_doDestroyRemote(gpointer data);

    gboolean doCreateRemote();
    gboolean doDestroyRemote();

    friend class RwControlRemote;
    void postMessage(RwControlMessage *msg);
};

class RwControlRemote {
public:
    RwControlRemote(GMainContext *mainContext, RwControlLocal *local);
    ~RwControlRemote();

    RwControlRemote(const RwControlRemote &) = delete;
    RwControlRemote &operator=(const RwControlRemote &) = delete;

private:
    GSource *       timer;
    GMainContext *  mainContext_;
    QMutex          m;
    RwControlLocal *local_;
    bool            start_requested;
    bool            blocking;
    bool            pending_status;

    RtpWorker *               worker;
    QList<RwControlMessage *> in;

    static gboolean cb_processMessages(gpointer data);
    static void     cb_worker_started(void *app);
    static void     cb_worker_updated(void *app);
    static void     cb_worker_stopped(void *app);
    static void     cb_worker_finished(void *app);
    static void     cb_worker_error(void *app);
    static void     cb_worker_audioOutputIntensity(int value, void *app);
    static void     cb_worker_audioInputIntensity(int value, void *app);
    static void     cb_worker_previewFrame(const RtpWorker::Frame &frame, void *app);
    static void     cb_worker_outputFrame(const RtpWorker::Frame &frame, void *app);
    static void     cb_worker_rtpAudioOut(const PRtpPacket &packet, void *app);
    static void     cb_worker_rtpVideoOut(const PRtpPacket &packet, void *app);
    static void     cb_worker_recordData(const QByteArray &packet, void *app);

    gboolean processMessages();
    void     worker_started();
    void     worker_updated();
    void     worker_stopped();
    void     worker_finished();
    void     worker_error();
    void     worker_audioOutputIntensity(int value);
    void     worker_audioInputIntensity(int value);
    void     worker_previewFrame(const RtpWorker::Frame &frame);
    void     worker_outputFrame(const RtpWorker::Frame &frame);
    void     worker_rtpAudioOut(const PRtpPacket &packet);
    void     worker_rtpVideoOut(const PRtpPacket &packet);
    void     worker_recordData(const QByteArray &packet);

    void resumeMessages();

    // return false to block further message processing
    bool processMessage(RwControlMessage *msg);

    friend class RwControlLocal;
    void postMessage(RwControlMessage *msg);
    void rtpAudioIn(const PRtpPacket &packet);
    void rtpVideoIn(const PRtpPacket &packet);
};

}

#endif
