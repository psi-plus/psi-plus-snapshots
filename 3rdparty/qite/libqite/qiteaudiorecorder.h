/*
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
*/

#ifndef QITEAUDIORECORDER_H
#define QITEAUDIORECORDER_H

#include <QObject>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
class QAudioRecorder;
#else
class QMediaRecorder;
class QMediaCaptureSession;
class QAudioInput;
#endif
class QAudioProbe;
class QTemporaryFile;
class QTimer;

class AudioRecorder : public QObject {
    Q_OBJECT
public:
    enum State { StoppedState, RecordingState };

    explicit AudioRecorder(QObject *parent = nullptr);

    void        record(); // for short-term records
    void        record(const QString &fileName);
    void        stop();
    inline void setMaxDuration(int ms) { _maxDuration = ms; } // set it before record() call or don't set at all

    inline auto    fileName() const { return _fileName; }
    inline auto    maxVolume() const { return _maxVolume; } // peak value of vlume over all the recording.
    inline auto    amplitudes() const { return _compressedHistorgram; }
    inline auto    data() const { return _audioData; }
    inline quint64 duration() const { return _duration; }
    inline State   state() const { return _state; }
    inline QString errorString() const { return _errorString; }

private:
    void cleanup();
    void postProcess(quint8 maxVolume, const QByteArray &amplitudes);
    void recordToFile(const QString &fileName);

signals:
    void finished(bool success);
public slots:

private:
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QAudioRecorder *_recorder = nullptr;
#else
    QAudioInput          *_audioInput     = nullptr;
    QMediaCaptureSession *_captureSession = nullptr;
    QMediaRecorder       *_recorder       = nullptr;
#endif
    QByteArray _compressedHistorgram;
    QString    _fileName;
    QByteArray _audioData;
    QTimer    *_maxDurationTimer = nullptr;
    qint64     _duration;
    int        _maxDuration = -1;
    bool       _isTmpFile   = false;
    quint8     _maxVolume   = 0;
    State      _state       = StoppedState;
    QString    _errorString;
};

#endif // QITEAUDIORECORDER_H
