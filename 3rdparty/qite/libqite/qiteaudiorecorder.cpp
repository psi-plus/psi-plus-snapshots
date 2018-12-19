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

#include "qiteaudiorecorder.h"
#include "qiteaudio.h"

#include <cmath>

#include <QAudioProbe>
#include <QAudioRecorder>
#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QMediaMetaData>
#include <QUrl>


template <typename T> struct SoloFrameDefault { enum { Default = 0 }; };

template <typename T> struct SoloFrame {

    SoloFrame()
        : data(T(SoloFrameDefault<T>::Default))
    {
    }

    SoloFrame(T sample)
        : data(sample)
    {
    }

    SoloFrame& operator=(const SoloFrame &other)
    {
        data = other.data;
        return *this;
    }

    T data;

    T average() const {return data;}
    void clear() {data = T(SoloFrameDefault<T>::Default);}
};

template<class T> struct PeakValue { static const T value = std::numeric_limits<T>::max(); };

template<> struct PeakValue <float> { static constexpr float value = float(1.00003); };

template<class T>
void handle(const QAudioBuffer &buffer, AudioRecorder::Quantum &quantum, QByteArray &collector, quint8 &maxVal) {
    const T *data = buffer.constData<T>();
    auto peakvalue = qreal(PeakValue<decltype(data[0].average())>::value);

    auto format = buffer.format();
    int countLeft = format.framesForDuration(quantum.timeLeft);
    Q_ASSERT(countLeft > 0);
    for (int i=0; i<buffer.frameCount(); i++) {
        quantum.sum += qreal(qAbs(data[i].average())) / peakvalue;
        quantum.count++;
        countLeft--;
        if (!countLeft) {
            auto value = quint8((quantum.sum / qreal(quantum.count)) * 255.0);
            if (value > maxVal) {
                maxVal = value;
            }
            //qDebug() << int((quantum.sum / qreal(quantum.count)) * 255.0);
            collector.append(char(value));
            if (collector.size() == collector.capacity()) {
                collector.reserve(collector.capacity() + AudioRecorder::HistogramMemSize);
            }
            quantum = AudioRecorder::Quantum();
            countLeft = format.framesForDuration(quantum.timeLeft);
        }
    }
    if (countLeft) {
        quantum.timeLeft = format.durationForFrames(countLeft);
    }
}


AudioRecorder::AudioRecorder(QObject *parent) : QObject(parent)
{
    _recorder = new QAudioRecorder(this);
    //qDebug() << "supported codecs for recorder:" << _recorder->supportedAudioCodecs();
    //qDebug() << "supported containers for recorder:" << _recorder->supportedContainers();

    probe = new QAudioProbe(this);
    probe->setSource(_recorder);

    QAudioEncoderSettings audioSettings;
    audioSettings.setCodec("audio/x-opus");
    audioSettings.setQuality(QMultimedia::HighQuality);

    _recorder->setEncodingSettings(audioSettings, QVideoEncoderSettings(), "audio/ogg");

    connect(_recorder, &QAudioRecorder::stateChanged, this, [this](){
        if (_recorder->state() == QAudioRecorder::StoppedState && _recorder->duration() && _maxVolume) {
            // compress histogram..
            auto volumeK = 255.0 / double(_maxVolume); // amplificator
            if (volumeK > 2) {
                volumeK = 2; // don't be mad on showing silence
            }
            auto step = histogram.size() / double(ITEAudioController::HistogramCompressedSize);
            QStringList columns;
            for (int i = 0; i < ITEAudioController::HistogramCompressedSize; i++) {
                int prev = int(step * i);
                int curr = int(step * (i + 1));
                if (curr == histogram.size()) {
                    curr = histogram.size() - 1;
                }

                int sum = 0;
                for (int j = prev; j <= curr; j++) {
                    sum += quint8(histogram[j]);
                }
                columns.append(QString::number(int(sum / double(curr - prev + 1) * volumeK)));
            }

            //qDebug() << columns.join(",");
            histogram.clear();
            histogram.squeeze();
#ifdef ITE_EMBED_HISTOGRAM // it's somewhat buggy with Qt since it not always writes metainfo at least in 5.11.2
            QFile f(_recorder->outputLocation().toLocalFile());
            QByteArray buffer;
            buffer.resize(4096 + 1024);
            qint64 lastPos = 0;
            if (f.open(QIODevice::ReadWrite)) {
                qint64 bytes;
                while ((bytes = f.read(buffer.data(), qint64(buffer.size()))) > 0) {
                    auto index = QByteArray::fromRawData(buffer.data(), int(bytes)).indexOf("AMPLDIAGSTART");
                    if (index >= 0) {
                        f.seek(lastPos + index + int(sizeof("AMPLDIAGSTART["))); // it's not a mistake with sizeof. It's [ is just escaped
                        f.write(columns.join(",").toLatin1().replace(',', "\\,"));
                        f.write("\\]AMPLDIAGEND");
                        f.flush();
                        break;
                    }
                    lastPos += 4096;
                    f.seek(lastPos);
                }
                f.close();
            }
#else
            QFile metaFile(_recorder->outputLocation().toLocalFile()+".histogram");
            if (metaFile.open(QIODevice::WriteOnly)) {
                metaFile.write(columns.join(",").toLatin1());
                metaFile.close();
            }
#endif
        }
        emit stateChanged();
    });

    connect(probe, &QAudioProbe::audioBufferProbed, this, [this](const QAudioBuffer &buffer){
        auto format = buffer.format();
        if (format.channelCount() > 2) {
            qWarning("unsupported amount of channels: %d", format.channelCount());
            return;
        }

        if (format.sampleType() == QAudioFormat::SignedInt) {
            switch (format.sampleSize()) {
            case 8:
                if(format.channelCount() == 2)
                    handle<QAudioBuffer::S8S>(buffer, quantum, histogram, _maxVolume);
                else
                    handle<SoloFrame<signed char>>(buffer, quantum, histogram, _maxVolume);
                break;
            case 16:
                if(format.channelCount() == 2)
                    handle<QAudioBuffer::S16S>(buffer, quantum, histogram, _maxVolume);
                else
                    handle<SoloFrame<signed short>>(buffer, quantum, histogram, _maxVolume);
                break;
            }
        } else if (format.sampleType() == QAudioFormat::UnSignedInt) {
            switch (format.sampleSize()) {
            case 8:
                if(format.channelCount() == 2)
                    handle<QAudioBuffer::S8U>(buffer, quantum, histogram, _maxVolume);
                else
                    handle<SoloFrame<unsigned char>>(buffer, quantum, histogram, _maxVolume);
                break;
            case 16:
                if(format.channelCount() == 2)
                    handle<QAudioBuffer::S16U>(buffer, quantum, histogram, _maxVolume);
                else
                    handle<SoloFrame<unsigned short>>(buffer, quantum, histogram, _maxVolume);
                break;
            }
        } else if(format.sampleType() == QAudioFormat::Float) {
            if(format.channelCount() == 2)
                handle<QAudioBuffer::S32F>(buffer, quantum, histogram, _maxVolume);
            else
                handle<SoloFrame<float>>(buffer, quantum, histogram, _maxVolume);
        } else {
            qWarning("unsupported audio sample type: %d", int(format.sampleType()));
        }
    });
}

void AudioRecorder::record(const QString &fileName)
{
    quantum = Quantum();
    histogram.clear();
    histogram.reserve(HistogramMemSize);
    _maxVolume = 0;
    _recorder->setOutputLocation(QUrl::fromLocalFile(fileName));
#ifdef ITE_EMBED_HISTOGRAM
    if (_recorder->isMetaDataWritable()) {
        auto reserved = QLatin1String("AMPLDIAGSTART[000") + QString(",000").repeated(ITEAudioController::HistogramCompressedSize-1) + QLatin1String("]AMPLDIAGEND");
        _recorder->setMetaData(QMediaMetaData::Comment, reserved);
    }
#endif
    _recorder->record();
}

void AudioRecorder::stop()
{
    _recorder->stop();
}
