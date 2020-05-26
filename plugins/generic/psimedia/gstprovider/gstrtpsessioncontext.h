/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
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

#ifndef PSIMEDIA_GSTRTPSESSIONCONTEXT_H
#define PSIMEDIA_GSTRTPSESSIONCONTEXT_H

#include "psimediaprovider.h"

#include "gstrecorder.h"
#include "gstrtpchannel.h"
#include "rwcontrol.h"

namespace PsiMedia {

class GstMainLoop;
class GstVideoWidget;

//----------------------------------------------------------------------------
// GstRtpSessionContext
//----------------------------------------------------------------------------
class GstRtpSessionContext : public QObject, public RtpSessionContext {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::RtpSessionContext)

public:
    GstMainLoop *gstLoop;

    RwControlLocal *       control;
    RwControlConfigDevices devices;
    RwControlConfigCodecs  codecs;
    RwControlTransmit      transmit;
    RwControlStatus        lastStatus;
    bool                   isStarted;
    bool                   isStopping;
    bool                   pending_status;

#ifdef QT_GUI_LIB
    GstVideoWidget *outputWidget, *previewWidget;
#endif

    GstRecorder recorder;

    // keep these parentless, so they can switch threads
    GstRtpChannel audioRtp;
    GstRtpChannel videoRtp;
    QMutex        write_mutex;
    bool          allow_writes;

    explicit GstRtpSessionContext(GstMainLoop *_gstLoop, QObject *parent = nullptr);

    ~GstRtpSessionContext() override;

    QObject *qobject() override;

    void cleanup();
    void setAudioOutputDevice(const QString &deviceId) override;
    void setAudioInputDevice(const QString &deviceId) override;
    void setVideoInputDevice(const QString &deviceId) override;
    void setFileInput(const QString &fileName) override;
    void setFileDataInput(const QByteArray &fileData) override;
    void setFileLoopEnabled(bool enabled) override;

#ifdef QT_GUI_LIB
    void setVideoOutputWidget(VideoWidgetContext *widget) override;
    void setVideoPreviewWidget(VideoWidgetContext *widget) override;
#endif

    void                setRecorder(QIODevice *recordDevice) override;
    void                stopRecording() override;
    void                setLocalAudioPreferences(const QList<PAudioParams> &params) override;
    void                setLocalVideoPreferences(const QList<PVideoParams> &params) override;
    void                setMaximumSendingBitrate(int kbps) override;
    void                setRemoteAudioPreferences(const QList<PPayloadInfo> &info) override;
    void                setRemoteVideoPreferences(const QList<PPayloadInfo> &info) override;
    void                start() override;
    void                updatePreferences() override;
    void                transmitAudio() override;
    void                transmitVideo() override;
    void                pauseAudio() override;
    void                pauseVideo() override;
    void                stop() override;
    QList<PPayloadInfo> localAudioPayloadInfo() const override;
    QList<PPayloadInfo> localVideoPayloadInfo() const override;
    QList<PPayloadInfo> remoteAudioPayloadInfo() const override;
    QList<PPayloadInfo> remoteVideoPayloadInfo() const override;
    QList<PAudioParams> audioParams() const override;
    QList<PVideoParams> videoParams() const override;
    bool                canTransmitAudio() const override;
    bool                canTransmitVideo() const override;
    int                 outputVolume() const override;
    void                setOutputVolume(int level) override;
    int                 inputVolume() const override;
    void                setInputVolume(int level) override;
    Error               errorCode() const override;
    RtpChannelContext * audioRtpChannel() override;
    RtpChannelContext * videoRtpChannel() override;
    void                dumpPipeline(std::function<void(const QStringList &)> callback) override;

    // channel calls this, which may be in another thread
    void push_packet_for_write(GstRtpChannel *from, const PRtpPacket &rtp);

signals:
    void started();
    void preferencesUpdated();
    void audioOutputIntensityChanged(int intensity);
    void audioInputIntensityChanged(int intensity);
    void stoppedRecording();
    void stopped();
    void finished();
    void error();

private slots:
    void control_statusReady(const RwControlStatus &status);
    void control_previewFrame(const QImage &img);
    void control_outputFrame(const QImage &img);
    void control_audioOutputIntensityChanged(int intensity);
    void control_audioInputIntensityChanged(int intensity);
    void recorder_stopped();

private:
    static void cb_control_rtpAudioOut(const PRtpPacket &packet, void *app);
    static void cb_control_rtpVideoOut(const PRtpPacket &packet, void *app);
    static void cb_control_recordData(const QByteArray &packet, void *app);

    // note: this is executed from a different thread
    void control_rtpAudioOut(const PRtpPacket &packet);

    // note: this is executed from a different thread
    void control_rtpVideoOut(const PRtpPacket &packet);

    // note: this is executed from a different thread
    void control_recordData(const QByteArray &packet);
};

} // namespace PsiMedia

#endif // PSIMEDIA_GSTRTPSESSIONCONTEXT_H
