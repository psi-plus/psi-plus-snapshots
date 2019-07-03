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

class QAudioRecorder;
class QAudioProbe;
class QTemporaryFile;
class QTimer;

class AudioRecorder : public QObject
{
    Q_OBJECT
public:
    static const qint64 HistogramQuantumSize = 10000; // 10ms. 100 values per second
    static const int HistogramMemSize = int(1e6) / HistogramQuantumSize * 20; // for 20 secs. ~ 2Kb

    struct Quantum {
        qint64 timeLeft = HistogramQuantumSize; // to generate next value for aplitude histogram
        qreal sum = 0.0;
        int count = 0;
    };

    explicit AudioRecorder(QObject *parent = nullptr);

    void record(); // for short-term records
    void record(const QString &fileName);
    void stop();
    inline void setMaxDuration(int ms) { _maxDuration = ms; } // set it before record() call or don't set at all

    inline auto recorder() const { return _recorder; }
    inline auto maxVolume() const { return _maxVolume; } // peak value of vlume over all the recording.
    inline auto histogram() const { return _compressedHistorgram; }
    inline auto data() const { return _audioData; }
    quint64 duration() const; // just a convenience method

private:
    void cleanup();
    void recordToFile(const QString &fileName);

signals:
    void stateChanged();
    void recorded();
public slots:

private:
    QAudioRecorder  *_recorder = nullptr;
    QAudioProbe     *_probe;
    Quantum         _quantum;
    QByteArray      _histogram;
    QByteArray      _compressedHistorgram;
    QByteArray      _audioData;
    QTimer          *_maxDurationTimer = nullptr;
    int             _maxDuration = -1;
    quint8          _maxVolume;
    bool            _destroying = false;
    bool            _isTmpFile = false;
};

#endif // QITEAUDIORECORDER_H
