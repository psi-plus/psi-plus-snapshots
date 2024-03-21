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

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMediaMetaData>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QAudioRecorder>
#define QtRecorder QAudioRecorder
#else
#include <QAudioDevice>
#include <QAudioInput>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#define QtRecorder QMediaRecorder
#endif

// #define QITE_DEBUG

namespace {
const qint64 HistogramQuantumSize = 10000;
const int    HistogramMemSize     = int(1e6) / HistogramQuantumSize * 20; // for 20 secs. ~ 2Kb

struct Quantum {
    qint64 timeLeft = HistogramQuantumSize; // to generate next value for aplitude amplitudes
    qreal  sum      = 0.0;
    int    count    = 0;
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
template <typename T> struct SoloFrameDefault {
    enum { Default = 0 };
};

template <typename T> struct SoloFrame {

    SoloFrame() : data(T(SoloFrameDefault<T>::Default)) { }

    SoloFrame(T sample) : data(sample) { }

    SoloFrame &operator=(const SoloFrame &other)
    {
        data = other.data;
        return *this;
    }

    T data;

    T    average() const { return data; }
    void clear() { data = T(SoloFrameDefault<T>::Default); }
};
#endif

}

class HistogramExtractor : public QObject {
    Q_OBJECT
public:
    HistogramExtractor(const QUrl &sourceUrl, QObject *parent) : QObject(parent)
    {
#ifdef QITE_DEBUG
        qDebug("Creating histogram extractor for %s", qPrintable(sourceUrl.toString()));
#endif
        auto localFile = sourceUrl.toLocalFile();
        if (!QFileInfo(localFile).exists()) {
            qFatal("Local file %s doesn't exist", qPrintable(localFile));
        }
        _amplitudes.reserve(HistogramMemSize);
        _decoder = new QAudioDecoder(this);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        _decoder->setSourceFilename(sourceUrl.toLocalFile());
#else
        _decoder->setSource(sourceUrl);
#endif
        connect(_decoder, &QAudioDecoder::bufferReady, this, &HistogramExtractor::bufferReady);
        connect(_decoder, &QAudioDecoder::finished, this, &HistogramExtractor::finished);
        connect(_decoder, &QAudioDecoder::finished, this, &HistogramExtractor::deleteLater);
    }

    void              start() { _decoder->start(); }
    inline quint8     maxVolume() const { return _maxVolume; }
    inline QByteArray amplitudes() const { return _amplitudes; }

signals:
    void finished();

private:
    template <class T> void handle(const QAudioBuffer &buffer)
    {
        auto     format = buffer.format();
        double   peakvalue; // unreachable value
        const T *data = buffer.constData<T>();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        using FrameDataType = decltype(data[0].average());
#else
        using FrameDataType = typename std::decay_t<decltype(data[0])>::value_type;
#endif
        if constexpr (std::is_floating_point_v<FrameDataType>) {
            peakvalue = 1.0003;
        } else { // integer
            if constexpr (std::is_signed_v<FrameDataType>) {
                peakvalue = double(std::numeric_limits<FrameDataType>::max()) + 1;
            } else {
                peakvalue = (double(std::numeric_limits<FrameDataType>::max()) + 1) / 2;
            }
        }

        int countLeft = format.framesForDuration(_quantum.timeLeft);
        Q_ASSERT(countLeft > 0);
        for (int i = 0; i < buffer.frameCount(); i++) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            auto average = qreal(qAbs(data[i].average()));
#else
            double sum = 0.0;
            for (auto value : data[i].channels) {
                if constexpr (std::is_floating_point_v<FrameDataType> || std::is_signed_v<FrameDataType>) {
                    sum += std::abs(double(value));
                } else {
                    sum += std::abs(double(value) - peakvalue);
                }
            }
            auto average = sum / double(std::size(data[i].channels)); // average over all channels
#endif
            // qDebug("%f / %f", average, peakvalue);
            _quantum.sum += average / peakvalue;
            _quantum.count++;
            countLeft--;
            if (!countLeft) {
                auto value = quint8((_quantum.sum / qreal(_quantum.count)) * 255.0);
                if (value > _maxVolume) {
                    _maxVolume = value;
                }
                // qDebug() << int((quantum.sum / qreal(quantum.count)) * 255.0);
                _amplitudes.append(char(value));
                if (_amplitudes.size() == _amplitudes.capacity()) {
                    _amplitudes.reserve(_amplitudes.capacity() + HistogramMemSize);
                }
                _quantum  = Quantum();
                countLeft = format.framesForDuration(_quantum.timeLeft);
            }
        }
        if (countLeft) {
            _quantum.timeLeft = format.durationForFrames(countLeft);
        }
    }

private slots:
    void bufferReady()
    {
        auto buffer = _decoder->read();
        auto format = buffer.format();
        if (format.channelCount() > 2) {
            qWarning("unsupported amount of channels: %d", format.channelCount());
            return;
        }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        if (format.sampleType() == QAudioFormat::SignedInt) {
            switch (format.sampleSize()) {
            case 8:
                if (format.channelCount() == 2)
                    handle<QAudioBuffer::S8S>(buffer);
                else
                    handle<SoloFrame<signed char>>(buffer);
                break;
            case 16:
                if (format.channelCount() == 2)
                    handle<QAudioBuffer::S16S>(buffer);
                else
                    handle<SoloFrame<signed short>>(buffer);
                break;
            }
        } else if (format.sampleType() == QAudioFormat::UnSignedInt) {
            switch (format.sampleSize()) {
            case 8:
                if (format.channelCount() == 2)
                    handle<QAudioBuffer::S8U>(buffer);
                else
                    handle<SoloFrame<unsigned char>>(buffer);
                break;
            case 16:
                if (format.channelCount() == 2)
                    handle<QAudioBuffer::S16U>(buffer);
                else
                    handle<SoloFrame<unsigned short>>(buffer);
                break;
            }
        } else if (format.sampleType() == QAudioFormat::Float) {
            if (format.channelCount() == 2)
                handle<QAudioBuffer::S32F>(buffer);
            else
                handle<SoloFrame<float>>(buffer);
        } else {
            qWarning("unsupported audio sample type: %d", int(format.sampleType()));
        }

#else
        switch (format.sampleFormat()) {
        case QAudioFormat::UInt8:
            if (format.channelCount() == 2)
                handle<QAudioBuffer::U8S>(buffer);
            else
                handle<QAudioBuffer::U8M>(buffer);
            break;
        case QAudioFormat::Int16:
            if (format.channelCount() == 2)
                handle<QAudioBuffer::S16S>(buffer);
            else
                handle<QAudioBuffer::S16M>(buffer);
            break;
        case QAudioFormat::Int32:
            if (format.channelCount() == 2)
                handle<QAudioBuffer::S32S>(buffer);
            else
                handle<QAudioBuffer::S32M>(buffer);
            break;
        case QAudioFormat::Float:
            if (format.channelCount() == 2)
                handle<QAudioBuffer::F32S>(buffer);
            else
                handle<QAudioBuffer::F32M>(buffer);
            break;
        default:
            qWarning("unsupported audio sample type: %d", int(format.sampleFormat()));
        }
#endif
    }

private:
    QAudioDecoder *_decoder;
    quint8         _maxVolume = 0;
    Quantum        _quantum;
    QByteArray     _amplitudes;
};

AudioRecorder::AudioRecorder(QObject *parent) : QObject(parent)
{
    _recorder = new QtRecorder(this);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QAudioEncoderSettings audioSettings;
    audioSettings.setCodec("audio/x-opus");
    audioSettings.setQuality(QMultimedia::HighQuality);

    //_recorder->setEncodingSettings(audioSettings, QVideoEncoderSettings(), "audio/ogg");
    _recorder->setEncodingSettings(audioSettings, QVideoEncoderSettings(), "video/quicktime, variant=(string)iso");
#else
    QMediaFormat mediaFormat(QMediaFormat::MPEG4);
    mediaFormat.setAudioCodec(QMediaFormat::AudioCodec::AAC);

    _audioInput = new QAudioInput(this);
    _audioInput->setDevice(QMediaDevices::defaultAudioInput());
    _audioInput->setMuted(false);

    _recorder->setQuality(QMediaRecorder::HighQuality);
    _recorder->setAudioChannelCount(1);
    _recorder->setMediaFormat(mediaFormat);

    _captureSession = new QMediaCaptureSession(this);
    _captureSession->setAudioInput(_audioInput);
    _captureSession->setRecorder(_recorder);
#endif

    connect(_recorder, &QtRecorder::durationChanged, this, [this](qint64 duration) { _duration = duration; });

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    connect(_recorder, &QtRecorder::stateChanged, this, [this]() {
        auto recorderState = _recorder->state();
#else
    connect(_recorder, &QtRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState recorderState) {
#endif
#ifdef QITE_DEBUG
        qDebug("State changed %d", recorderState);
#endif
        if (recorderState == QtRecorder::StoppedState) {
            if (_maxDurationTimer && _maxDurationTimer->isActive()) {
                delete _maxDurationTimer;
                _maxDurationTimer = nullptr;
            }
            if (_recorder->error() == QtRecorder::NoError) {
                auto he = new HistogramExtractor(_recorder->outputLocation(), this); // it's self deletable
                connect(he, &HistogramExtractor::finished, this,
                        [he, this]() { postProcess(he->maxVolume(), he->amplitudes()); });
                he->start();
                return;
            }
            _errorString = _recorder->errorString();
            _state       = StoppedState;
            emit finished(false);
        } else if (recorderState == QtRecorder::RecordingState) {
            _state = RecordingState;
        }
    });

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    connect(_recorder, static_cast<void (QMediaRecorder::*)(QMediaRecorder::Error error)>(&QMediaRecorder::error), this,
            [this](QMediaRecorder::Error error) {
                _errorString = _recorder->errorString();
#else
    connect(_recorder, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error error, const QString &errorString) {
                _errorString = errorString;
#endif
                Q_UNUSED(error);
                if (_state == RecordingState) {
                    return; // will report error on StoppedState
                }
                _state = StoppedState;
                emit this->finished(false);
            });
}

void AudioRecorder::record()
{
    cleanup();
    _isTmpFile = true;

    QTemporaryFile *tmpFile = new QTemporaryFile(QDir::tempPath() + QLatin1String("/qite-record-XXXXXX.mp4"), this);
    tmpFile->setAutoRemove(false);
    tmpFile->open();
    QString fn = tmpFile->fileName();
    delete tmpFile;

    recordToFile(fn);
}

void AudioRecorder::record(const QString &fileName)
{
    cleanup();
    _fileName = QFileInfo(fileName).absoluteFilePath();
    recordToFile(_fileName);
}

void AudioRecorder::recordToFile(const QString &fileName)
{
    _recorder->setOutputLocation(QUrl::fromLocalFile(fileName));
#ifdef ITE_EMBED_HISTOGRAM
    if (_recorder->isMetaDataWritable()) {
        auto reserved = QLatin1String("AMPLDIAGSTART[000")
            + QString(",000").repeated(ITEAudioController::HistogramCompressedSize - 1) + QLatin1String("]AMPLDIAGEND");
        _recorder->setMetaData(QMediaMetaData::Comment, reserved);
    }
#endif
    if (_maxDuration != -1) {
        _maxDurationTimer = new QTimer(this);
        _maxDurationTimer->setSingleShot(true);
        _maxDurationTimer->setInterval(_maxDuration);
        connect(_maxDurationTimer, &QTimer::timeout, this, &AudioRecorder::stop);
    }
#ifdef QITE_DEBUG
    qDebug("start recording to %s", qPrintable(fileName));
#endif
    _recorder->record();
}

void AudioRecorder::stop()
{
    _duration = _recorder->duration();
    _recorder->stop();
}

void AudioRecorder::cleanup()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (_recorder->state() == QtRecorder::RecordingState)
#else
    if (_recorder->recorderState() == QtRecorder::RecordingState)
#endif
        _recorder->stop();
    _isTmpFile = false;
    _compressedHistorgram.clear();
    _audioData.clear();
    _audioData.squeeze();
    if (_maxDurationTimer) {
        delete _maxDurationTimer;
        _maxDurationTimer = nullptr;
    }
    _state = StoppedState;
    _errorString.clear();
    _maxVolume = 0;
}

void AudioRecorder::postProcess(quint8 maxVolume, const QByteArray &amplitudes)
{
    _maxVolume = maxVolume;
    if (!_maxVolume) {
        _errorString = QLatin1String("Silence recorded");
        _state       = StoppedState;
        emit finished(false);
        return;
    }
    // compress amplitudes..
    auto volumeK = 255.0 / double(_maxVolume); // amplificator
    if (volumeK > 8) {
        volumeK = 8; // don't be mad on showing silence
    }
    auto step = amplitudes.size() / double(ITEAudioController::HistogramCompressedSize);
    _compressedHistorgram.reserve(ITEAudioController::HistogramCompressedSize);

    for (int i = 0; i < ITEAudioController::HistogramCompressedSize; i++) {
        int prev = int(step * i);
        int curr = int(step * (i + 1));
        if (curr == amplitudes.size()) {
            curr = amplitudes.size() - 1;
        }

        int sum = 0;
        for (int j = prev; j <= curr; j++) {
            sum += quint8(amplitudes[j]);
        }
        _compressedHistorgram.append(int(sum / double(curr - prev + 1) * volumeK));
    }

    QStringList columns;
    std::transform(_compressedHistorgram.begin(), _compressedHistorgram.end(), std::back_inserter(columns),
                   [](auto const &v) { return QString::number(v); });

    // qDebug() << columns.join(",");
#ifdef ITE_EMBED_HISTOGRAM // it's somewhat buggy with Qt since it not always writes metainfo at least in 5.11.2
    QFile      f(_recorder->outputLocation().toLocalFile());
    QByteArray buffer;
    buffer.resize(4096 + 1024);
    qint64 lastPos = 0;
    if (f.open(QIODevice::ReadWrite)) {
        qint64 bytes;
        while ((bytes = f.read(buffer.data(), qint64(buffer.size()))) > 0) {
            auto index = QByteArray::fromRawData(buffer.data(), int(bytes)).indexOf("AMPLDIAGSTART");
            if (index >= 0) {
                f.seek(lastPos + index
                       + int(sizeof("AMPLDIAGSTART["))); // it's not a mistake with sizeof. It's [ is just escaped
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
    if (_isTmpFile) {
        QString fn = _recorder->outputLocation().toLocalFile();
        QFile   f(fn);
        f.open(QIODevice::ReadOnly);
        _audioData = f.readAll();
        f.close();
        f.remove();
    } else {
        QFile metaFile(_recorder->outputLocation().toLocalFile() + ".amplitudes");
        if (metaFile.open(QIODevice::WriteOnly)) {
            metaFile.write(columns.join(",").toLatin1());
            metaFile.close();
        }
    }
    _state = StoppedState;
    emit finished(true);
#endif
}

#include "qiteaudiorecorder.moc"
