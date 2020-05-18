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

#include "psimedia.h"
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QLibrary>
#include <QMainWindow>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <QVariant>
#include <QtPlugin>

#include "main.h"

#define BASE_PORT_MIN 1
#define BASE_PORT_MAX 65534

static QString urlishEncode(const QString &in)
{
    QString out;
    for (int n = 0; n < in.length(); ++n) {
        if (in[n] == '%' || in[n] == ',' || in[n] == ';' || in[n] == ':' || in[n] == '\n') {
            unsigned char c = quint8(in[n].toLatin1());
            out += QString("%%1").arg(c, 2, 16, QLatin1Char('0'));
        } else
            out += in[n];
    }
    return out;
}

static QString urlishDecode(const QString &in)
{
    QString out;
    for (int n = 0; n < in.length(); ++n) {
        if (in[n] == '%') {
            if (n + 2 >= in.length())
                return QString();

            QString hex = in.mid(n + 1, 2);
            bool    ok;
            int     x = hex.toInt(&ok, 16);
            if (!ok)
                return QString();

            unsigned char c = quint8(x);
            out += char(c);
            n += 2;
        } else
            out += in[n];
    }
    return out;
}

static QString payloadInfoToString(const PsiMedia::PayloadInfo &info)
{
    QStringList list;
    list += QString::number(info.id());
    list += info.name();
    list += QString::number(info.clockrate());
    list += QString::number(info.channels());
    list += QString::number(info.ptime());
    list += QString::number(info.maxptime());
    foreach (const PsiMedia::PayloadInfo::Parameter &p, info.parameters())
        list += p.name + '=' + p.value;

    for (int n = 0; n < list.count(); ++n)
        list[n] = urlishEncode(list[n]);
    return list.join(",");
}

static PsiMedia::PayloadInfo stringToPayloadInfo(const QString &in)
{
    QStringList list = in.split(',');
    if (list.count() < 6)
        return PsiMedia::PayloadInfo();

    for (int n = 0; n < list.count(); ++n) {
        QString str = urlishDecode(list[n]);
        if (str.isEmpty())
            return PsiMedia::PayloadInfo();
        list[n] = str;
    }

    PsiMedia::PayloadInfo out;
    bool                  ok;
    int                   x;

    x = list[0].toInt(&ok);
    if (!ok)
        return PsiMedia::PayloadInfo();
    out.setId(x);

    out.setName(list[1]);

    x = list[2].toInt(&ok);
    if (!ok)
        return PsiMedia::PayloadInfo();
    out.setClockrate(x);

    x = list[3].toInt(&ok);
    if (!ok)
        return PsiMedia::PayloadInfo();
    out.setChannels(x);

    x = list[4].toInt(&ok);
    if (!ok)
        return PsiMedia::PayloadInfo();
    out.setPtime(x);

    x = list[5].toInt(&ok);
    if (!ok)
        return PsiMedia::PayloadInfo();
    out.setMaxptime(x);

    QList<PsiMedia::PayloadInfo::Parameter> plist;
    for (int n = 6; n < list.count(); ++n) {
        x = list[n].indexOf('=');
        if (x == -1)
            return PsiMedia::PayloadInfo();
        PsiMedia::PayloadInfo::Parameter p;
        p.name  = list[n].mid(0, x);
        p.value = list[n].mid(x + 1);
        plist += p;
    }
    out.setParameters(plist);

    return out;
}

static QString payloadInfoToCodecString(const PsiMedia::PayloadInfo *audio, const PsiMedia::PayloadInfo *video)
{
    QStringList list;
    if (audio)
        list += QString("A:") + payloadInfoToString(*audio);
    if (video)
        list += QString("V:") + payloadInfoToString(*video);
    return list.join(";");
}

static bool codecStringToPayloadInfo(const QString &in, PsiMedia::PayloadInfo *audio, PsiMedia::PayloadInfo *video)
{
    QStringList list = in.split(';');
    foreach (const QString &s, list) {
        int x = s.indexOf(':');
        if (x == -1)
            return false;

        QString var = s.mid(0, x);
        QString val = s.mid(x + 1);
        if (val.isEmpty())
            return false;

        PsiMedia::PayloadInfo info = stringToPayloadInfo(val);
        if (info.isNull())
            return false;

        if (var == "A" && audio)
            *audio = info;
        else if (var == "V" && video)
            *video = info;
    }

    return true;
}

ConfigDlg::ConfigDlg(MainWin *parent) : QDialog(parent)
{
    ui.setupUi(this);
    setWindowTitle(tr("Configure Audio/Video"));

    // with qt-5.7 and above we can use qOverlaod instead of these awful casts
    connect(ui.cb_audioInDevice, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int) { hasAudioInPref = true; });
    connect(ui.cb_audioOutDevice, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int) { hasAudioOutPref = true; });
    connect(ui.cb_videoInDevice, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int) { hasVideoInPref = true; });
    connect(ui.cb_audioMode, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int) { hasAudioParams = true; });
    connect(ui.cb_videoMode, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
            [this](int) { hasVideoParams = true; });

    ui.lb_audioInDevice->setEnabled(false);
    ui.cb_audioInDevice->setEnabled(false);
    ui.lb_videoInDevice->setEnabled(false);
    ui.cb_videoInDevice->setEnabled(false);
    ui.lb_file->setEnabled(false);
    ui.le_file->setEnabled(false);
    ui.tb_file->setEnabled(false);
    ui.ck_loop->setEnabled(false);
    featuresWatcher = parent->featureWatcher;

    connect(ui.rb_sendLive, SIGNAL(toggled(bool)), SLOT(live_toggled(bool)));
    connect(ui.rb_sendFile, SIGNAL(toggled(bool)), SLOT(file_toggled(bool)));
    connect(ui.tb_file, SIGNAL(clicked()), SLOT(file_choose()));
    connect(featuresWatcher, &FeaturesWatcher::updated, this, &ConfigDlg::featuresUpdated);

    featuresUpdated();
}

void ConfigDlg::featuresUpdated()
{
    ui.cb_audioInDevice->blockSignals(true);
    ui.cb_audioOutDevice->blockSignals(true);
    ui.cb_videoInDevice->blockSignals(true);
    ui.cb_audioMode->blockSignals(true);
    ui.cb_videoMode->blockSignals(true);

    QString audioInPref  = ui.cb_audioInDevice->currentData().toString();
    QString audioOutPref = ui.cb_audioOutDevice->currentData().toString();
    QString videoInPref  = ui.cb_videoInDevice->currentData().toString();
    auto    audioMode    = ui.cb_audioMode->currentData().value<PsiMedia::AudioParams>();
    auto    videoMode    = ui.cb_videoMode->currentData().value<PsiMedia::VideoParams>();

    ui.cb_audioOutDevice->clear();
    ui.cb_audioOutDevice->addItem("<None>", QString());
    for (const auto &dev : featuresWatcher->audioOutputDevices())
        ui.cb_audioOutDevice->addItem(dev.name(), dev.id());

    ui.cb_audioInDevice->clear();
    ui.cb_audioInDevice->addItem("<None>", QString());
    for (const auto &dev : featuresWatcher->audioInputDevices())
        ui.cb_audioInDevice->addItem(dev.name(), dev.id());

    ui.cb_videoInDevice->clear();
    ui.cb_videoInDevice->addItem("<None>", QString());
    for (const auto &dev : featuresWatcher->videoInputDevices())
        ui.cb_videoInDevice->addItem(dev.name(), dev.id());

    ui.cb_audioMode->clear();
    foreach (const PsiMedia::AudioParams &params, featuresWatcher->supportedAudioModes()) {
        QString codec = params.codec();
        if (codec == "vorbis" || codec == "opus")
            codec[0] = codec[0].toUpper();
        else
            codec = codec.toUpper();
        QString hz = QString::number(params.sampleRate() / 1000);
        QString chanstr;
        if (params.channels() == 1)
            chanstr = "Mono";
        else if (params.channels() == 2)
            chanstr = "Stereo";
        else
            chanstr = QString("Channels: %1").arg(params.channels());
        QString str = QString("%1, %2KHz, %3-bit, %4").arg(codec).arg(hz).arg(params.sampleSize()).arg(chanstr);

        ui.cb_audioMode->addItem(str, QVariant::fromValue<PsiMedia::AudioParams>(params));
    }

    ui.cb_videoMode->clear();
    foreach (const PsiMedia::VideoParams &params, featuresWatcher->supportedVideoModes()) {
        QString codec = params.codec();
        if (codec == "theora")
            codec[0] = codec[0].toUpper();
        else
            codec = codec.toUpper();
        QString sizestr = QString("%1x%2").arg(params.size().width()).arg(params.size().height());
        QString str     = QString("%1, %2 @ %3fps").arg(codec).arg(sizestr).arg(params.fps());

        ui.cb_videoMode->addItem(str, QVariant::fromValue<PsiMedia::VideoParams>(params));
    }

    // the following lookups are guaranteed, since the config is
    //   adjusted to all valid values as necessary
    auto config = featuresWatcher->configuration();
    if (!hasAudioInPref)
        audioInPref = config.audioInDeviceId; // if we didn't change anything, try default from config
    if (!hasAudioOutPref)
        audioOutPref = config.audioOutDeviceId;
    if (!hasVideoInPref)
        videoInPref = config.videoInDeviceId;
    if (!hasAudioParams)
        audioMode = config.audioParams;
    if (!hasVideoParams)
        videoMode = config.videoParams;
    ui.cb_audioOutDevice->setCurrentIndex(ui.cb_audioOutDevice->findData(audioOutPref));
    ui.cb_audioInDevice->setCurrentIndex(ui.cb_audioInDevice->findData(audioInPref));
    ui.cb_videoInDevice->setCurrentIndex(ui.cb_videoInDevice->findData(videoInPref));
    ui.cb_audioMode->setCurrentIndex(findAudioParamsData(ui.cb_audioMode, audioMode));
    ui.cb_videoMode->setCurrentIndex(findVideoParamsData(ui.cb_videoMode, videoMode));
    if (config.liveInput)
        ui.rb_sendLive->setChecked(true);
    else
        ui.rb_sendFile->setChecked(true);
    ui.le_file->setText(config.file);
    ui.ck_loop->setChecked(config.loopFile);

    ui.cb_audioInDevice->blockSignals(false);
    ui.cb_audioOutDevice->blockSignals(false);
    ui.cb_videoInDevice->blockSignals(false);
    ui.cb_audioMode->blockSignals(false);
    ui.cb_videoMode->blockSignals(false);
}

// apparently custom QVariants can't be compared, so we have to
//   make our own find functions for the comboboxes
int ConfigDlg::findAudioParamsData(QComboBox *cb, const PsiMedia::AudioParams &params)
{
    for (int n = 0; n < cb->count(); ++n) {
        if (cb->itemData(n).value<PsiMedia::AudioParams>() == params)
            return n;
    }

    return -1;
}

int ConfigDlg::findVideoParamsData(QComboBox *cb, const PsiMedia::VideoParams &params)
{
    for (int n = 0; n < cb->count(); ++n) {
        if (cb->itemData(n).value<PsiMedia::VideoParams>() == params)
            return n;
    }

    return -1;
}

void ConfigDlg::accept()
{
    QSettings s;
    if (hasAudioInPref)
        s.setValue("audioIn", ui.cb_audioInDevice->itemData(ui.cb_audioInDevice->currentIndex()).toString());
    if (hasAudioOutPref)
        s.setValue("audioOut", ui.cb_audioOutDevice->itemData(ui.cb_audioOutDevice->currentIndex()).toString());
    if (hasVideoInPref)
        s.setValue("videoIn", ui.cb_videoInDevice->itemData(ui.cb_videoInDevice->currentIndex()).toString());
    if (hasAudioParams)
        s.setValue(
            "audioParams",
            ui.cb_audioMode->itemData(ui.cb_audioMode->currentIndex()).value<PsiMedia::AudioParams>().toString());
    if (hasVideoParams)
        s.setValue(
            "videoParams",
            ui.cb_videoMode->itemData(ui.cb_videoMode->currentIndex()).value<PsiMedia::VideoParams>().toString());

    s.setValue("liveInput", ui.rb_sendLive->isChecked());
    s.setValue("file", ui.le_file->text());
    s.setValue("loopFile", ui.ck_loop->isChecked());

    featuresWatcher->updateDefaults();

    QDialog::accept();
}

void ConfigDlg::live_toggled(bool on)
{
    ui.lb_audioInDevice->setEnabled(on);
    ui.cb_audioInDevice->setEnabled(on);
    ui.lb_videoInDevice->setEnabled(on);
    ui.cb_videoInDevice->setEnabled(on);
}

void ConfigDlg::file_toggled(bool on)
{
    ui.lb_file->setEnabled(on);
    ui.le_file->setEnabled(on);
    ui.tb_file->setEnabled(on);
    ui.ck_loop->setEnabled(on);
}

void ConfigDlg::file_choose()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), QCoreApplication::applicationDirPath(),
                                                    tr("Ogg Audio/Video (*.oga *.ogv *.ogg)"));
    if (!fileName.isEmpty())
        ui.le_file->setText(fileName);
}

RtpSocketGroup::RtpSocketGroup(QObject *parent) : QObject(parent)
{
    connect(&socket[0], SIGNAL(readyRead()), SLOT(sock_readyRead()));
    connect(&socket[1], SIGNAL(readyRead()), SLOT(sock_readyRead()));
    connect(&socket[0], SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));
    connect(&socket[1], SIGNAL(bytesWritten(qint64)), SLOT(sock_bytesWritten(qint64)));
}

bool RtpSocketGroup::bind(int basePort)
{
    if (!socket[0].bind(quint16(basePort)))
        return false;
    return socket[1].bind(quint16(basePort + 1));
}

void RtpSocketGroup::sock_readyRead()
{
    auto udp = static_cast<QUdpSocket *>(sender());
    if (udp == &socket[0])
        emit readyRead(0);
    else
        emit readyRead(1);
}

void RtpSocketGroup::sock_bytesWritten(qint64 bytes)
{
    Q_UNUSED(bytes);

    QUdpSocket *udp = static_cast<QUdpSocket *>(sender());
    if (udp == &socket[0])
        emit datagramWritten(0);
    else
        emit datagramWritten(1);
}

RtpBinding::RtpBinding(Mode _mode, PsiMedia::RtpChannel *_channel, RtpSocketGroup *_socketGroup, QObject *parent) :
    QObject(parent), mode(_mode), channel(_channel), socketGroup(_socketGroup), sendBasePort(-1)
{
    socketGroup->setParent(this);
    connect(socketGroup, SIGNAL(readyRead(int)), SLOT(net_ready(int)));
    connect(socketGroup, SIGNAL(datagramWritten(int)), SLOT(net_written(int)));
    connect(channel, SIGNAL(readyRead()), SLOT(app_ready()));
    connect(channel, SIGNAL(packetsWritten(int)), SLOT(app_written(int)));
}

void RtpBinding::net_ready(int offset)
{
    // here we handle packets received from the network, that
    //   we need to give to psimedia

    while (socketGroup->socket[offset].hasPendingDatagrams()) {
        int        size = int(socketGroup->socket[offset].pendingDatagramSize());
        QByteArray rawValue;
        rawValue.resize(size);
        QHostAddress fromAddr;
        quint16      fromPort;
        if (socketGroup->socket[offset].readDatagram(rawValue.data(), size, &fromAddr, &fromPort) == -1)
            continue;

        // if we are sending RTP, we should not be receiving
        //   anything on offset 0
        if (mode == Send && offset == 0)
            continue;

        PsiMedia::RtpPacket packet(rawValue, offset);
        channel->write(packet);
    }
}

void RtpBinding::net_written(int offset)
{
    Q_UNUSED(offset);
    // do nothing
}

void RtpBinding::app_ready()
{
    // here we handle packets that psimedia wants to send out,
    //   that we need to give to the network

    while (channel->packetsAvailable() > 0) {
        PsiMedia::RtpPacket packet = channel->read();
        int                 offset = packet.portOffset();
        if (offset < 0 || offset > 1)
            continue;

        // if we are receiving RTP, we should not be sending
        //   anything on offset 0
        if (mode == Receive && offset == 0)
            continue;

        if (sendAddress.isNull() || sendBasePort < BASE_PORT_MIN || sendBasePort > BASE_PORT_MAX)
            continue;

        socketGroup->socket[offset].writeDatagram(packet.rawValue(), sendAddress, quint16(sendBasePort + offset));
    }
}

void RtpBinding::app_written(int count)
{
    Q_UNUSED(count);
    // do nothing
}

MainWin::MainWin() :
    action_AboutProvider(nullptr), producer(this), receiver(this), sendAudioRtp(nullptr), sendVideoRtp(nullptr),
    receiveAudioRtp(nullptr), receiveVideoRtp(nullptr), recording(false), recordFile(nullptr)
{
    ui.setupUi(this);
    setWindowTitle(tr("PsiMedia Demo"));

    creditName = PsiMedia::creditName();
    if (!creditName.isEmpty()) {
        action_AboutProvider = new QAction(this);
        action_AboutProvider->setText(tr("About %1").arg(creditName));
        ui.menu_Help->addAction(action_AboutProvider);
        connect(action_AboutProvider, SIGNAL(triggered()), SLOT(doAboutProvider()));
    }

    featureWatcher = new FeaturesWatcher(this);
    connect(featureWatcher, &FeaturesWatcher::updated, this, &MainWin::featuresUpdated);

    ui.pb_transmit->setEnabled(false);
    ui.pb_stopSend->setEnabled(false);
    ui.pb_stopReceive->setEnabled(false);
    ui.pb_record->setEnabled(false);
    ui.le_sendConfig->setReadOnly(true);
    ui.lb_sendConfig->setEnabled(false);
    ui.le_sendConfig->setEnabled(false);
    ui.sl_mic->setMinimum(0);
    ui.sl_mic->setMaximum(100);
    ui.sl_spk->setMinimum(0);
    ui.sl_spk->setMaximum(100);
    ui.sl_mic->setValue(100);
    ui.sl_spk->setValue(100);

    ui.le_remoteAddress->setText("127.0.0.1");
    ui.le_remoteAudioPort->setText("60000");
    ui.le_remoteVideoPort->setText("60002");
    ui.le_localAudioPort->setText("60000");
    ui.le_localVideoPort->setText("60002");
    ui.le_remoteAddress->selectAll();
    ui.le_remoteAddress->setFocus();

    connect(ui.action_Quit, SIGNAL(triggered()), SLOT(close()));
    connect(ui.action_Configure, SIGNAL(triggered()), SLOT(doConfigure()));
    connect(ui.action_About, SIGNAL(triggered()), SLOT(doAbout()));
    connect(ui.pb_startSend, SIGNAL(clicked()), SLOT(start_send()));
    connect(ui.pb_transmit, SIGNAL(clicked()), SLOT(transmit()));
    connect(ui.pb_stopSend, SIGNAL(clicked()), SLOT(stop_send()));
    connect(ui.pb_startReceive, SIGNAL(clicked()), SLOT(start_receive()));
    connect(ui.pb_stopReceive, SIGNAL(clicked()), SLOT(stop_receive()));
    connect(ui.pb_record, SIGNAL(clicked()), SLOT(record_toggle()));
    connect(ui.sl_mic, SIGNAL(valueChanged(int)), SLOT(change_volume_mic(int)));
    connect(ui.sl_spk, SIGNAL(valueChanged(int)), SLOT(change_volume_spk(int)));
    connect(&producer, SIGNAL(started()), SLOT(producer_started()));
    connect(&producer, SIGNAL(stopped()), SLOT(producer_stopped()));
    connect(&producer, SIGNAL(finished()), SLOT(producer_finished()));
    connect(&producer, SIGNAL(error()), SLOT(producer_error()));
    connect(&receiver, SIGNAL(started()), SLOT(receiver_started()));
    connect(&receiver, SIGNAL(stoppedRecording()), SLOT(receiver_stoppedRecording()));
    connect(&receiver, SIGNAL(stopped()), SLOT(receiver_stopped()));
    connect(&receiver, SIGNAL(error()), SLOT(receiver_error()));

    // set initial volume levels
    change_volume_mic(ui.sl_mic->value());
    change_volume_spk(ui.sl_spk->value());

    // associate video widgets
    producer.setVideoPreviewWidget(ui.vw_self);
    receiver.setVideoOutputWidget(ui.vw_remote);

    // hack: make the top/bottom layouts have matching height
    int  lineEditHeight = ui.le_receiveConfig->sizeHint().height();
    auto spacer         = new QWidget(this);
    spacer->setMinimumHeight(lineEditHeight);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui.gridLayout2->addWidget(spacer, 3, 1);

    // hack: give the video widgets a 4:3 ratio
    int gridSpacing = ui.gridLayout1->verticalSpacing();
    if (gridSpacing == -1)
        gridSpacing = 9; // not sure how else to get this
    int pushButtonHeight = ui.pb_startSend->sizeHint().height();
    int heightEstimate   = lineEditHeight * 4 + pushButtonHeight + gridSpacing * 4;
    heightEstimate += 10; // pad just to be safe
    int goodWidth = (heightEstimate * 4) / 3;
    ui.vw_remote->setMinimumSize(goodWidth, heightEstimate);
    ui.vw_self->setMinimumSize(goodWidth, heightEstimate);

    // hack: remove empty File menu on mac
#ifdef Q_WS_MAC
    ui.menu_File->menuAction()->setVisible(false);
#endif
}

MainWin::~MainWin()
{
    producer.reset();
    receiver.reset();

    cleanup_send_rtp();
    cleanup_receive_rtp();
    cleanup_record();
}

void MainWin::featuresUpdated()
{
    // TODO
}

void MainWin::setSendFieldsEnabled(bool b)
{
    ui.lb_remoteAddress->setEnabled(b);
    ui.le_remoteAddress->setEnabled(b);
    ui.lb_remoteAudioPort->setEnabled(b);
    ui.le_remoteAudioPort->setEnabled(b);
    ui.lb_remoteVideoPort->setEnabled(b);
    ui.le_remoteVideoPort->setEnabled(b);
}

void MainWin::setSendConfig(const QString &s)
{
    if (!s.isEmpty()) {
        ui.lb_sendConfig->setEnabled(true);
        ui.le_sendConfig->setEnabled(true);
        ui.le_sendConfig->setText(s);
        ui.le_sendConfig->setCursorPosition(0);
        if (ui.le_receiveConfig->text().isEmpty())
            ui.le_receiveConfig->setText(s);
    } else {
        ui.lb_sendConfig->setEnabled(false);
        ui.le_sendConfig->setEnabled(false);
        ui.le_sendConfig->clear();
    }
}

void MainWin::setReceiveFieldsEnabled(bool b)
{
    ui.lb_localAudioPort->setEnabled(b);
    ui.le_localAudioPort->setEnabled(b);
    ui.lb_localVideoPort->setEnabled(b);
    ui.le_localVideoPort->setEnabled(b);
    ui.lb_receiveConfig->setEnabled(b);
    ui.le_receiveConfig->setEnabled(b);
}

QString MainWin::rtpSessionErrorToString(PsiMedia::RtpSession::Error e)
{
    QString str;
    switch (e) {
    case PsiMedia::RtpSession::ErrorSystem:
        str = tr("System error");
        break;
    case PsiMedia::RtpSession::ErrorCodec:
        str = tr("Codec error");
        break;
    default: // generic
        str = tr("Generic error");
        break;
    }
    return str;
}

void MainWin::cleanup_send_rtp()
{
    delete sendAudioRtp;
    sendAudioRtp = nullptr;
    delete sendVideoRtp;
    sendVideoRtp = nullptr;
}

void MainWin::cleanup_receive_rtp()
{
    delete receiveAudioRtp;
    receiveAudioRtp = nullptr;
    delete receiveVideoRtp;
    receiveVideoRtp = nullptr;
}

void MainWin::cleanup_record()
{
    if (recording) {
        delete recordFile;
        recordFile = nullptr;
        recording  = false;
    }
}

void MainWin::doConfigure()
{
    ConfigDlg w(this);
    w.exec();
}

void MainWin::doAbout()
{
    QMessageBox::about(this, tr("About PsiMedia Demo"),
                       tr("PsiMedia Demo v1.0\n"
                          "A simple test application for the PsiMedia system.\n"
                          "\n"
                          "Copyright (C) 2008  Barracuda Networks, Inc."));
}

void MainWin::doAboutProvider() { QMessageBox::about(this, tr("About %1").arg(creditName), PsiMedia::creditText()); }

void MainWin::start_send()
{
    transmitAudio = false;
    transmitVideo = false;

    auto config = featureWatcher->configuration();
    if (config.liveInput) {
        if (config.audioInDeviceId.isEmpty() && config.videoInDeviceId.isEmpty()) {
            QMessageBox::information(this, tr("Error"),
                                     tr("Cannot send live without at least one audio "
                                        "input or video input device selected."));
            return;
        }

        if (!config.audioInDeviceId.isEmpty()) {
            producer.setAudioInputDevice(config.audioInDeviceId);
            transmitAudio = true;
        } else
            producer.setAudioInputDevice(QString());

        if (!config.videoInDeviceId.isEmpty()) {
            producer.setVideoInputDevice(config.videoInDeviceId);

            transmitVideo = true;
        } else
            producer.setVideoInputDevice(QString());
    } else // non-live (file) input
    {
        producer.setFileInput(config.file);
        producer.setFileLoopEnabled(config.loopFile);

        // we just assume the file has both audio and video.
        //   if it doesn't, no big deal, it'll still work.
        //   update: after producer is started, we can correct
        //   these variables.
        transmitAudio = true;
        transmitVideo = true;
    }

    QList<PsiMedia::AudioParams> audioParamsList;
    if (transmitAudio)
        audioParamsList += config.audioParams;
    producer.setLocalAudioPreferences(audioParamsList);

    QList<PsiMedia::VideoParams> videoParamsList;
    if (transmitVideo)
        videoParamsList += config.videoParams;
    producer.setLocalVideoPreferences(videoParamsList);

    ui.pb_startSend->setEnabled(false);
    ui.pb_stopSend->setEnabled(true);
    transmitting = false;
    producer.start();
}

void MainWin::transmit()
{
    QHostAddress addr;
    if (!addr.setAddress(ui.le_remoteAddress->text())) {
        QMessageBox::critical(this, tr("Error"), tr("Invalid send IP address."));
        return;
    }

    int audioPort = -1;
    if (transmitAudio) {
        bool ok;
        audioPort = ui.le_remoteAudioPort->text().toInt(&ok);
        if (!ok || audioPort < BASE_PORT_MIN || audioPort > BASE_PORT_MAX) {
            QMessageBox::critical(this, tr("Error"), tr("Invalid send audio port."));
            return;
        }
    }

    int videoPort = -1;
    if (transmitVideo) {
        bool ok;
        videoPort = ui.le_remoteVideoPort->text().toInt(&ok);
        if (!ok || videoPort < BASE_PORT_MIN || videoPort > BASE_PORT_MAX) {
            QMessageBox::critical(this, tr("Error"), tr("Invalid send video port."));
            return;
        }
    }

    auto audioSocketGroup      = new RtpSocketGroup;
    sendAudioRtp               = new RtpBinding(RtpBinding::Send, producer.audioRtpChannel(), audioSocketGroup, this);
    sendAudioRtp->sendAddress  = addr;
    sendAudioRtp->sendBasePort = audioPort;

    auto videoSocketGroup      = new RtpSocketGroup;
    sendVideoRtp               = new RtpBinding(RtpBinding::Send, producer.videoRtpChannel(), videoSocketGroup, this);
    sendVideoRtp->sendAddress  = addr;
    sendVideoRtp->sendBasePort = videoPort;

    setSendFieldsEnabled(false);
    ui.pb_transmit->setEnabled(false);

    if (transmitAudio)
        producer.transmitAudio();
    if (transmitVideo)
        producer.transmitVideo();

    transmitting = true;
}

void MainWin::stop_send()
{
    ui.pb_stopSend->setEnabled(false);

    if (!transmitting)
        ui.pb_transmit->setEnabled(false);

    producer.stop();
}

void MainWin::start_receive()
{
    QString               receiveConfig = ui.le_receiveConfig->text();
    PsiMedia::PayloadInfo audio;
    PsiMedia::PayloadInfo video;
    if (receiveConfig.isEmpty() || !codecStringToPayloadInfo(receiveConfig, &audio, &video)) {
        QMessageBox::critical(this, tr("Error"), tr("Invalid codec config."));
        return;
    }

    receiveAudio = !audio.isNull();
    receiveVideo = !video.isNull();

    int audioPort = -1;
    if (receiveAudio) {
        bool ok;
        audioPort = ui.le_localAudioPort->text().toInt(&ok);
        if (!ok || audioPort < BASE_PORT_MIN || audioPort > BASE_PORT_MAX) {
            QMessageBox::critical(this, tr("Error"), tr("Invalid receive audio port."));
            return;
        }
    }

    int videoPort = -1;
    if (receiveVideo) {
        bool ok;
        videoPort = ui.le_localVideoPort->text().toInt(&ok);
        if (!ok || videoPort < BASE_PORT_MIN || videoPort > BASE_PORT_MAX) {
            QMessageBox::critical(this, tr("Error"), tr("Invalid receive video port."));
            return;
        }
    }

    auto config = featureWatcher->configuration();
    if (receiveAudio && !config.audioOutDeviceId.isEmpty()) {
        receiver.setAudioOutputDevice(config.audioOutDeviceId);

        QList<PsiMedia::AudioParams> audioParamsList;
        audioParamsList += config.audioParams;
        receiver.setLocalAudioPreferences(audioParamsList);

        QList<PsiMedia::PayloadInfo> payloadInfoList;
        payloadInfoList += audio;
        receiver.setRemoteAudioPreferences(payloadInfoList);
    }

    if (receiveVideo) {
        QList<PsiMedia::VideoParams> videoParamsList;
        videoParamsList += config.videoParams;
        receiver.setLocalVideoPreferences(videoParamsList);

        QList<PsiMedia::PayloadInfo> payloadInfoList;
        payloadInfoList += video;
        receiver.setRemoteVideoPreferences(payloadInfoList);
    }

    auto audioSocketGroup = new RtpSocketGroup(this);
    auto videoSocketGroup = new RtpSocketGroup(this);
    if (!audioSocketGroup->bind(audioPort)) {
        delete audioSocketGroup;
        audioSocketGroup = nullptr;
        delete videoSocketGroup;
        videoSocketGroup = nullptr;

        QMessageBox::critical(this, tr("Error"), tr("Unable to bind to receive audio ports."));
        return;
    }
    if (!videoSocketGroup->bind(videoPort)) {
        delete audioSocketGroup;
        audioSocketGroup = nullptr;
        delete videoSocketGroup;
        videoSocketGroup = nullptr;

        QMessageBox::critical(this, tr("Error"), tr("Unable to bind to receive video ports."));
        return;
    }

    receiveAudioRtp = new RtpBinding(RtpBinding::Receive, receiver.audioRtpChannel(), audioSocketGroup, this);
    receiveVideoRtp = new RtpBinding(RtpBinding::Receive, receiver.videoRtpChannel(), videoSocketGroup, this);

    setReceiveFieldsEnabled(false);
    ui.pb_startReceive->setEnabled(false);
    ui.pb_stopReceive->setEnabled(true);
    receiver.start();
}

void MainWin::stop_receive()
{
    ui.pb_stopReceive->setEnabled(false);
    receiver.stop();
}

void MainWin::change_volume_mic(int value) { producer.setInputVolume(value); }

void MainWin::change_volume_spk(int value) { receiver.setOutputVolume(value); }

void MainWin::producer_started()
{
    PsiMedia::PayloadInfo audio, *pAudio;
    PsiMedia::PayloadInfo video, *pVideo;

    pAudio = nullptr;
    pVideo = nullptr;
    if (transmitAudio) {
        // confirm transmitting of audio is actually possible,
        //   in the case that a file is used as input
        if (producer.canTransmitAudio()) {
            audio  = producer.localAudioPayloadInfo().first();
            pAudio = &audio;
        } else
            transmitAudio = false;
    }
    if (transmitVideo) {
        // same for video
        if (producer.canTransmitVideo()) {
            video  = producer.localVideoPayloadInfo().first();
            pVideo = &video;
        } else
            transmitVideo = false;
    }

    QString str = payloadInfoToCodecString(pAudio, pVideo);
    setSendConfig(str);

    ui.pb_transmit->setEnabled(true);
}

void MainWin::producer_stopped()
{
    cleanup_send_rtp();

    setSendFieldsEnabled(true);
    setSendConfig(QString());
    ui.pb_startSend->setEnabled(true);
}

void MainWin::producer_finished()
{
    cleanup_send_rtp();

    setSendFieldsEnabled(true);
    setSendConfig(QString());
    ui.pb_startSend->setEnabled(true);
    ui.pb_transmit->setEnabled(false);
    ui.pb_stopSend->setEnabled(false);
}

void MainWin::producer_error()
{
    cleanup_send_rtp();

    setSendFieldsEnabled(true);
    setSendConfig(QString());
    ui.pb_startSend->setEnabled(true);
    ui.pb_transmit->setEnabled(false);
    ui.pb_stopSend->setEnabled(false);

    QMessageBox::critical(
        this, tr("Error"),
        tr("An error occurred while trying to send:\n%1.").arg(rtpSessionErrorToString(producer.errorCode())));
}

void MainWin::receiver_started() { ui.pb_record->setEnabled(true); }

void MainWin::receiver_stoppedRecording() { cleanup_record(); }

void MainWin::receiver_stopped()
{
    cleanup_receive_rtp();
    cleanup_record();

    setReceiveFieldsEnabled(true);
    ui.pb_startReceive->setEnabled(true);
    ui.pb_record->setEnabled(false);
}

void MainWin::receiver_error()
{
    cleanup_receive_rtp();
    cleanup_record();

    setReceiveFieldsEnabled(true);
    ui.pb_startReceive->setEnabled(true);
    ui.pb_stopReceive->setEnabled(false);
    ui.pb_record->setEnabled(false);

    QMessageBox::critical(
        this, tr("Error"),
        tr("An error occurred while trying to receive:\n%1.").arg(rtpSessionErrorToString(receiver.errorCode())));
}

void MainWin::record_toggle()
{
    if (!recording) {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"), QDir::homePath(),
                                                        tr("Ogg Audio/Video (*.oga *.ogv)"));
        if (fileName.isEmpty())
            return;

        recordFile = new QFile(fileName, this);
        if (!recordFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            delete recordFile;

            QMessageBox::critical(this, tr("Error"), tr("Unable to create file for recording."));
            return;
        }

        receiver.setRecordingQIODevice(recordFile);
        recording = true;
    } else
        receiver.stopRecording();
}

#ifdef GSTPROVIDER_STATIC
Q_IMPORT_PLUGIN(gstprovider)
#endif

#ifndef GSTPROVIDER_STATIC
static QString findPlugin(const QString &relpath, const QString &basename)
{
    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.cd(relpath))
        return QString();
    foreach (const QString &fileName, dir.entryList()) {
        if (fileName.contains(basename)) {
            QString filePath = dir.filePath(fileName);
            if (QLibrary::isLibrary(filePath))
                return filePath;
        }
    }
    return QString();
}
#endif

int main(int argc, char **argv)
{
    QApplication qapp(argc, argv);

    QApplication::setOrganizationName("psi-im.org");
    QApplication::setApplicationName("psimedia");

#ifndef GSTPROVIDER_STATIC
    QString pluginFile;
    QString resourcePath;

    pluginFile = qgetenv("PSI_MEDIA_PLUGIN");
    if (pluginFile.isEmpty()) {
#if defined(Q_OS_WIN)
        pluginFile = findPlugin(".", "gstprovider" DEBUG_POSTFIX);
        if (!pluginFile.isEmpty())
            resourcePath = QCoreApplication::applicationDirPath() + "/gstreamer-1.0";
#elif defined(Q_OS_MAC)
        pluginFile = findPlugin("../PlugIns", "gstprovider" DEBUG_POSTFIX);
        // codesign can't sign gstreamer-1.0 folder
        if (!pluginFile.isEmpty())
            resourcePath = QCoreApplication::applicationDirPath() + "/../PlugIns/gstreamer";
#endif

        if (pluginFile.isEmpty())
            pluginFile = findPlugin("../gstprovider", "gstprovider" DEBUG_POSTFIX);

#ifdef PLUGIN_INSTALL_PATH
        if (pluginFile.isEmpty())
            pluginFile = findPlugin(PLUGIN_INSTALL_PATH, "gstprovider" DEBUG_POSTFIX);
#endif
#ifdef PLUGIN_INSTALL_PATH_DEBUG
        if (pluginFile.isEmpty())
            pluginFile = findPlugin(PLUGIN_INSTALL_PATH_DEBUG, "gstprovider" DEBUG_POSTFIX);
#endif
    }

    PsiMedia::loadPlugin(pluginFile, resourcePath);
#endif

    if (!PsiMedia::isSupported()) {
        QMessageBox::critical(nullptr, MainWin::tr("PsiMedia Demo"),
                              MainWin::tr("Error: Could not load PsiMedia subsystem."));
        return 1;
    }

    MainWin mainWin;

    // give mainWin a chance to fix its layout before showing
    QTimer::singleShot(0, &mainWin, SLOT(show()));

    QApplication::exec();
    return 0;
}

//----------------------------------------------------
// FeaturesWatcher
//----------------------------------------------------
FeaturesWatcher::FeaturesWatcher(QObject *parent)
{
    Q_UNUSED(parent);
    QSettings s;

    connect(&_features, &PsiMedia::Features::updated, this, &FeaturesWatcher::featuresUpdated);
    updateDefaults();
}

FeaturesWatcher::~FeaturesWatcher() = default;

void FeaturesWatcher::updateDefaults()
{
    QSettings s;

    _configuration.liveInput = s.value("liveInput", true).toBool();
    _configuration.loopFile  = s.value("liveFile", true).toBool();
    _configuration.file      = s.value("file", QString()).toString();

    bool    hasAudioIn       = s.contains("audioIn"); // if we ever opened configuration dialog and set something there
    bool    hasAudioOut      = s.contains("audioOut");
    bool    hasVideoIn       = s.contains("videoIn");
    QString userPrefAudioIn  = s.value("audioIn").toString();
    QString userPrefAudioOut = s.value("audioOut").toString();
    QString userPrefVideoIn  = s.value("videoIn").toString();
    QString audioParams      = s.value("audioParams").toString();
    QString videoParams      = s.value("videoParams").toString();

    _configuration.audioInDeviceId = (hasAudioIn && userPrefAudioIn.isEmpty())
        ? QString()
        : defaultDeviceId(_features.audioInputDevices(), userPrefAudioIn);
    _configuration.audioOutDeviceId = (hasAudioOut && userPrefAudioOut.isEmpty())
        ? QString()
        : defaultDeviceId(_features.audioOutputDevices(), userPrefAudioOut);
    _configuration.videoInDeviceId = (hasVideoIn && userPrefVideoIn.isEmpty())
        ? QString()
        : defaultDeviceId(_features.videoInputDevices(), userPrefVideoIn);

    bool found = false;
    for (auto const &d : _features.supportedAudioModes()) {
        if (d.toString() == audioParams) {
            _configuration.audioParams = d;
            found                      = true;
            break;
        }
    }
    if (!found && _features.supportedAudioModes().count())
        _configuration.audioParams = _features.supportedAudioModes().first();

    found = false;
    for (auto const &d : _features.supportedVideoModes()) {
        if (d.toString() == videoParams) {
            _configuration.videoParams = d;
            found                      = true;
            break;
        }
    }
    if (!found && _features.supportedVideoModes().count())
        _configuration.videoParams = _features.supportedVideoModes().first();
}

void FeaturesWatcher::featuresUpdated()
{
    updateDefaults();
    emit updated();
}

QString FeaturesWatcher::defaultDeviceId(const QList<PsiMedia::Device> &devs, const QString &userPref)
{
    QString def;
    bool    userPrefFound = false;
    for (auto const &d : devs) {
        if (d.isDefault())
            def = d.id();
        if (d.id() == userPref) {
            userPrefFound = true;
            break;
        }
    }

    if (userPrefFound)
        return userPref;

    if (def.isEmpty() && !devs.isEmpty()) {
        def = devs.first().id();
    }

    return def;
}
