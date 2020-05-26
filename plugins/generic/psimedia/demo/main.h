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

#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QHostAddress>
#include <QMainWindow>
#include <QUdpSocket>

#include "ui_config.h"
#include "ui_mainwin.h"

#include <psimedia.h>

class Configuration {
public:
    bool                  liveInput;
    QString               audioOutDeviceId, audioInDeviceId, videoInDeviceId;
    QString               file;
    bool                  loopFile;
    PsiMedia::AudioParams audioParams;
    PsiMedia::VideoParams videoParams;

    Configuration() : liveInput(false), loopFile(false) { }
};

class FeaturesWatcher : public QObject {
    Q_OBJECT

    Configuration      _configuration;
    PsiMedia::Features _features;

    QString defaultDeviceId(const QList<PsiMedia::Device> &devs, const QString &userPref);

public:
    explicit FeaturesWatcher(QObject *parent);
    ~FeaturesWatcher() override;
    inline const Configuration &        configuration() const { return _configuration; }
    inline const PsiMedia::Features &   features() const { return _features; }
    inline QList<PsiMedia::Device>      audioInputDevices() { return _features.audioInputDevices(); }
    inline QList<PsiMedia::Device>      audioOutputDevices() { return _features.audioOutputDevices(); }
    inline QList<PsiMedia::Device>      videoInputDevices() { return _features.videoInputDevices(); }
    inline QList<PsiMedia::AudioParams> supportedAudioModes() { return _features.supportedAudioModes(); }
    inline QList<PsiMedia::VideoParams> supportedVideoModes() { return _features.supportedVideoModes(); }

    void updateDefaults();
private slots:
    void featuresUpdated();

signals:
    void updated();
};

class MainWin;
class ConfigDlg : public QDialog {
    Q_OBJECT

public:
    Ui::Config       ui;
    FeaturesWatcher *featuresWatcher;
    bool             hasAudioInPref  = false;
    bool             hasAudioOutPref = false;
    bool             hasVideoInPref  = false;
    bool             hasAudioParams  = false;
    bool             hasVideoParams  = false;

    explicit ConfigDlg(MainWin *parent = nullptr);
    int findAudioParamsData(QComboBox *cb, const PsiMedia::AudioParams &params);
    int findVideoParamsData(QComboBox *cb, const PsiMedia::VideoParams &params);

protected:
    void accept() override;

private slots:
    void live_toggled(bool on);
    void file_toggled(bool on);
    void file_choose();
    void featuresUpdated();
};

// handles two udp sockets
class RtpSocketGroup : public QObject {
    Q_OBJECT

public:
    QUdpSocket socket[2];

    explicit RtpSocketGroup(QObject *parent = nullptr);
    bool bind(int basePort);

signals:
    void readyRead(int offset);
    void datagramWritten(int offset);

private slots:
    void sock_readyRead();
    void sock_bytesWritten(qint64 bytes);
};

// bind a channel to a socket group.
// takes ownership of socket group.
class RtpBinding : public QObject {
    Q_OBJECT

public:
    enum Mode { Send, Receive };

    Mode                  mode;
    PsiMedia::RtpChannel *channel;
    RtpSocketGroup *      socketGroup;
    QHostAddress          sendAddress;
    int                   sendBasePort;

    RtpBinding(Mode _mode, PsiMedia::RtpChannel *_channel, RtpSocketGroup *_socketGroup, QObject *parent = nullptr);

private slots:
    void net_ready(int offset);
    void net_written(int offset);
    void app_ready();
    void app_written(int count);
};

class MainWin : public QMainWindow {
    Q_OBJECT

public:
    Ui::MainWin          ui;
    QAction *            action_AboutProvider;
    QString              creditName;
    PsiMedia::RtpSession producer;
    PsiMedia::RtpSession receiver;
    bool                 transmitAudio, transmitVideo, transmitting;
    bool                 receiveAudio, receiveVideo;
    RtpBinding *         sendAudioRtp, *sendVideoRtp;
    RtpBinding *         receiveAudioRtp, *receiveVideoRtp;
    bool                 recording;
    QFile *              recordFile;
    FeaturesWatcher *    featureWatcher;

    MainWin();
    ~MainWin() override;
    void           setSendFieldsEnabled(bool b);
    void           setSendConfig(const QString &s);
    void           setReceiveFieldsEnabled(bool b);
    static QString rtpSessionErrorToString(PsiMedia::RtpSession::Error e);
    void           cleanup_send_rtp();
    void           cleanup_receive_rtp();
    void           cleanup_record();

private slots:
    void doConfigure();
    void doAbout();
    void doAboutProvider();
    void start_send();
    void transmit();
    void stop_send();
    void start_receive();
    void stop_receive();
    void change_volume_mic(int value);
    void change_volume_spk(int value);
    void producer_started();
    void producer_stopped();
    void producer_finished();
    void producer_error();
    void receiver_started();
    void receiver_stoppedRecording();
    void receiver_stopped();
    void receiver_error();
    void record_toggle();
    void featuresUpdated();
    void doShowPipeline();
};
