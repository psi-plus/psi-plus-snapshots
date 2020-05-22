#include "gstaudiorecordercontext.h"

namespace PsiMedia {

GstAudioRecorderContext::GstAudioRecorderContext(GstMainLoop *_gstLoop, QObject *parent) :
    QObject(parent), gstLoop(_gstLoop)
{
}

GstAudioRecorderContext::~GstAudioRecorderContext() { }

QObject *GstAudioRecorderContext::qobject() { return this; }

void GstAudioRecorderContext::setInputDevice(const QString &deviceId)
{
    Q_UNUSED(deviceId);
#if 0
    devices.audioInId = deviceId;
    devices.fileNameIn.clear();
    devices.fileDataIn.clear();
    if (control)
        control->updateDevices(devices);
#endif
}

void GstAudioRecorderContext::setOutputDevice(QIODevice *recordDevice) { Q_UNUSED(recordDevice); }

void GstAudioRecorderContext::setPreferences(const QList<PAudioParams> &params) { Q_UNUSED(params); }

QList<PAudioParams> GstAudioRecorderContext::preferences() const { return QList<PAudioParams>(); }

void GstAudioRecorderContext::start() { }

void GstAudioRecorderContext::pause() { }

void GstAudioRecorderContext::stop() { }

AudioRecorderContext::Error GstAudioRecorderContext::errorCode() const { return ErrorGeneric; }

} // namespace PsiMedia
