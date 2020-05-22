#include "gstrtpsessioncontext.h"

#include "gstthread.h"
#ifdef QT_GUI_LIB
#include "gstvideowidget.h"
#endif

namespace PsiMedia {

GstRtpSessionContext::GstRtpSessionContext(GstMainLoop *_gstLoop, QObject *parent) :
    QObject(parent), gstLoop(_gstLoop), control(nullptr), isStarted(false), isStopping(false), pending_status(false),
    recorder(this), allow_writes(false)
{
#ifdef QT_GUI_LIB
    outputWidget  = nullptr;
    previewWidget = nullptr;
#endif

    devices.audioOutVolume = 100;
    devices.audioInVolume  = 100;

    codecs.useLocalAudioParams = true;
    codecs.useLocalVideoParams = true;

    audioRtp.session = this;
    videoRtp.session = this;

    connect(&recorder, SIGNAL(stopped()), SLOT(recorder_stopped()));
}

GstRtpSessionContext::~GstRtpSessionContext() { cleanup(); }

QObject *GstRtpSessionContext::qobject() { return this; }

void GstRtpSessionContext::cleanup()
{
    if (outputWidget)
        outputWidget->show_frame(QImage());
    if (previewWidget)
        previewWidget->show_frame(QImage());

    codecs = RwControlConfigCodecs();

    isStarted      = false;
    isStopping     = false;
    pending_status = false;

    recorder.control = nullptr;

    write_mutex.lock();
    allow_writes = false;
    delete control;
    control = nullptr;
    write_mutex.unlock();
}

void GstRtpSessionContext::setAudioOutputDevice(const QString &deviceId)
{
    devices.audioOutId = deviceId;
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setAudioInputDevice(const QString &deviceId)
{
    devices.audioInId = deviceId;
    devices.fileNameIn.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setVideoInputDevice(const QString &deviceId)
{
    devices.videoInId = deviceId;
    devices.fileNameIn.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setFileInput(const QString &fileName)
{
    devices.fileNameIn = fileName;
    devices.audioInId.clear();
    devices.videoInId.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setFileDataInput(const QByteArray &fileData)
{
    devices.fileDataIn = fileData;
    devices.audioInId.clear();
    devices.videoInId.clear();
    devices.fileNameIn.clear();
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setFileLoopEnabled(bool enabled)
{
    devices.loopFile = enabled;
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setVideoOutputWidget(VideoWidgetContext *widget)
{
    // no change?
    if (!outputWidget && !widget)
        return;
    if (outputWidget && outputWidget->context == widget)
        return;

    delete outputWidget;
    outputWidget = nullptr;

    if (widget)
        outputWidget = new GstVideoWidget(widget, this);

    devices.useVideoOut = widget != nullptr;
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setVideoPreviewWidget(VideoWidgetContext *widget)
{
    // no change?
    if (!previewWidget && !widget)
        return;
    if (previewWidget && previewWidget->context == widget)
        return;

    delete previewWidget;
    previewWidget = nullptr;

    if (widget)
        previewWidget = new GstVideoWidget(widget, this);

    devices.useVideoPreview = widget != nullptr;
    if (control)
        control->updateDevices(devices);
}

void GstRtpSessionContext::setRecorder(QIODevice *recordDevice)
{
    // can't assign a new recording device after stopping
    Q_ASSERT(!isStopping);

    recorder.setDevice(recordDevice);
}

void GstRtpSessionContext::stopRecording() { recorder.stop(); }

void GstRtpSessionContext::setLocalAudioPreferences(const QList<PAudioParams> &params)
{
    codecs.useLocalAudioParams = true;
    codecs.localAudioParams    = params;
}

void GstRtpSessionContext::setLocalVideoPreferences(const QList<PVideoParams> &params)
{
    codecs.useLocalVideoParams = true;
    codecs.localVideoParams    = params;
}

void GstRtpSessionContext::setMaximumSendingBitrate(int kbps) { codecs.maximumSendingBitrate = kbps; }

void GstRtpSessionContext::setRemoteAudioPreferences(const QList<PPayloadInfo> &info)
{
    codecs.useRemoteAudioPayloadInfo = true;
    codecs.remoteAudioPayloadInfo    = info;
}

void GstRtpSessionContext::setRemoteVideoPreferences(const QList<PPayloadInfo> &info)
{
    codecs.useRemoteVideoPayloadInfo = true;
    codecs.remoteVideoPayloadInfo    = info;
}

void GstRtpSessionContext::start()
{
    Q_ASSERT(!control && !isStarted);

    write_mutex.lock();

    control = new RwControlLocal(gstLoop, this);
    connect(control, SIGNAL(statusReady(const RwControlStatus &)), SLOT(control_statusReady(const RwControlStatus &)));
    connect(control, SIGNAL(previewFrame(const QImage &)), SLOT(control_previewFrame(const QImage &)));
    connect(control, SIGNAL(outputFrame(const QImage &)), SLOT(control_outputFrame(const QImage &)));
    connect(control, SIGNAL(audioOutputIntensityChanged(int)), SLOT(control_audioOutputIntensityChanged(int)));
    connect(control, SIGNAL(audioInputIntensityChanged(int)), SLOT(control_audioInputIntensityChanged(int)));

    control->app            = this;
    control->cb_rtpAudioOut = cb_control_rtpAudioOut;
    control->cb_rtpVideoOut = cb_control_rtpVideoOut;
    control->cb_recordData  = cb_control_recordData;

    allow_writes = true;
    write_mutex.unlock();

    recorder.control = control;

    lastStatus     = RwControlStatus();
    isStarted      = false;
    pending_status = true;
    control->start(devices, codecs);
}

void GstRtpSessionContext::updatePreferences()
{
    Q_ASSERT(control && !pending_status);

    pending_status = true;
    control->updateCodecs(codecs);
}

void GstRtpSessionContext::transmitAudio()
{
    transmit.useAudio = true;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::transmitVideo()
{
    transmit.useVideo = true;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::pauseAudio()
{
    transmit.useAudio = false;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::pauseVideo()
{
    transmit.useVideo = false;
    control->setTransmit(transmit);
}

void GstRtpSessionContext::stop()
{
    Q_ASSERT(control && !isStopping);

    // note: it's possible to stop even if pending_status is
    //   already true.  this is so we can stop a session that
    //   is in the middle of starting.

    isStopping     = true;
    pending_status = true;
    control->stop();
}

QList<PPayloadInfo> GstRtpSessionContext::localAudioPayloadInfo() const { return lastStatus.localAudioPayloadInfo; }

QList<PPayloadInfo> GstRtpSessionContext::localVideoPayloadInfo() const { return lastStatus.localVideoPayloadInfo; }

QList<PPayloadInfo> GstRtpSessionContext::remoteAudioPayloadInfo() const { return lastStatus.remoteAudioPayloadInfo; }

QList<PPayloadInfo> GstRtpSessionContext::remoteVideoPayloadInfo() const { return lastStatus.remoteVideoPayloadInfo; }

QList<PAudioParams> GstRtpSessionContext::audioParams() const { return lastStatus.localAudioParams; }

QList<PVideoParams> GstRtpSessionContext::videoParams() const { return lastStatus.localVideoParams; }

bool GstRtpSessionContext::canTransmitAudio() const { return lastStatus.canTransmitAudio; }

bool GstRtpSessionContext::canTransmitVideo() const { return lastStatus.canTransmitVideo; }

int GstRtpSessionContext::outputVolume() const { return devices.audioOutVolume; }

void GstRtpSessionContext::setOutputVolume(int level)
{
    devices.audioOutVolume = level;
    if (control)
        control->updateDevices(devices);
}

int GstRtpSessionContext::inputVolume() const { return devices.audioInVolume; }

void GstRtpSessionContext::setInputVolume(int level)
{
    devices.audioInVolume = level;
    if (control)
        control->updateDevices(devices);
}

RtpSessionContext::Error GstRtpSessionContext::errorCode() const { return static_cast<Error>(lastStatus.errorCode); }

RtpChannelContext *GstRtpSessionContext::audioRtpChannel() { return &audioRtp; }

RtpChannelContext *GstRtpSessionContext::videoRtpChannel() { return &videoRtp; }

void GstRtpSessionContext::push_packet_for_write(GstRtpChannel *from, const PRtpPacket &rtp)
{
    QMutexLocker locker(&write_mutex);
    if (!allow_writes || !control)
        return;

    if (from == &audioRtp)
        control->rtpAudioIn(rtp);
    else if (from == &videoRtp)
        control->rtpVideoIn(rtp);
}

void GstRtpSessionContext::control_statusReady(const RwControlStatus &status)
{
    lastStatus = status;

    if (status.finished) {
        // finished status just means the file is done
        //   sending.  the session still remains active.
        emit finished();
    } else if (status.error) {
        cleanup();
        emit error();
    } else if (pending_status) {
        if (status.stopped) {
            pending_status = false;

            cleanup();
            emit stopped();
            return;
        }

        // if we're currently stopping, ignore all other
        //   pending status events except for stopped
        //   (handled above)
        if (isStopping)
            return;

        pending_status = false;

        if (!isStarted) {
            isStarted = true;

            // if there was a pending record, start it
            recorder.startNext();

            emit started();
        } else
            emit preferencesUpdated();
    }
}

void GstRtpSessionContext::control_previewFrame(const QImage &img)
{
    if (previewWidget)
        previewWidget->show_frame(img);
}

void GstRtpSessionContext::control_outputFrame(const QImage &img)
{
    if (outputWidget)
        outputWidget->show_frame(img);
}

void GstRtpSessionContext::control_audioOutputIntensityChanged(int intensity)
{
    emit audioOutputIntensityChanged(intensity);
}

void GstRtpSessionContext::control_audioInputIntensityChanged(int intensity)
{
    emit audioInputIntensityChanged(intensity);
}

void GstRtpSessionContext::recorder_stopped() { emit stoppedRecording(); }

void GstRtpSessionContext::cb_control_rtpAudioOut(const PRtpPacket &packet, void *app)
{
    static_cast<GstRtpSessionContext *>(app)->control_rtpAudioOut(packet);
}

void GstRtpSessionContext::cb_control_rtpVideoOut(const PRtpPacket &packet, void *app)
{
    static_cast<GstRtpSessionContext *>(app)->control_rtpVideoOut(packet);
}

void GstRtpSessionContext::cb_control_recordData(const QByteArray &packet, void *app)
{
    static_cast<GstRtpSessionContext *>(app)->control_recordData(packet);
}

void GstRtpSessionContext::control_rtpAudioOut(const PRtpPacket &packet) { audioRtp.push_packet_for_read(packet); }

void GstRtpSessionContext::control_rtpVideoOut(const PRtpPacket &packet) { videoRtp.push_packet_for_read(packet); }

void GstRtpSessionContext::control_recordData(const QByteArray &packet) { recorder.push_data_for_read(packet); }

} // namespace PsiMedia
