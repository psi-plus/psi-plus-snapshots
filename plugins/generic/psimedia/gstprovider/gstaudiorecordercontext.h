#ifndef PSIMEDIA_GSTAUDIORECORDERCONTEXT_H
#define PSIMEDIA_GSTAUDIORECORDERCONTEXT_H

#include "psimediaprovider.h"

namespace PsiMedia {

class GstMainLoop;

class GstAudioRecorderContext : public QObject, public AudioRecorderContext {
    Q_OBJECT
    Q_INTERFACES(PsiMedia::AudioRecorderContext)

public:
    GstMainLoop *gstLoop;

    bool isStarted      = false;
    bool isStopping     = false;
    bool pending_status = false;

    explicit GstAudioRecorderContext(GstMainLoop *_gstLoop, QObject *parent = nullptr);
    ~GstAudioRecorderContext() override;

    QObject *qobject() override;

    // AudioRecorderContext interface
public:
    void                setInputDevice(const QString &deviceId) override;
    void                setOutputDevice(QIODevice *recordDevice) override;
    void                setPreferences(const QList<PAudioParams> &params) override;
    QList<PAudioParams> preferences() const override;
    void                start() override;
    void                pause() override;
    void                stop() override;
    Error               errorCode() const override;
};

} // namespace PsiMedia

#endif // PSIMEDIA_GSTAUDIORECORDERCONTEXT_H
