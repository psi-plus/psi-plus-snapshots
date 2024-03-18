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

#include "mainwindow.h"
#include "qite.h"
#include "qiteaudio.h"
#include "qiteaudiorecorder.h"
#include "ui_mainwindow.h"

#include <QAction>
#include <QAudioRecorder>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QIcon>
#include <QStandardPaths>
#include <QStyle>
#include <ctime>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    recordAction = new QAction(QIcon(":/icon/recorder-microphone.png"), "Record", this);
    ui->mainToolBar->addAction(recordAction);
    connect(recordAction, &QAction::triggered, this, &MainWindow::recordMic);

    auto fontCombo = new QComboBox(this);
    for (int i = 8; i < 26; i++) {
        fontCombo->addItem(QString::number(i));
        auto comboAct = ui->mainToolBar->addWidget(fontCombo);
        fontCombo->setCurrentText(QString::number(ui->textEdit->fontPointSize()));
        comboAct->setVisible(true);
        connect(fontCombo, &QComboBox::currentTextChanged, this,
                [this](const QString &text) { ui->textEdit->setFontPointSize(text.toInt()); });
    }

    auto itc = new InteractiveText(ui->textEdit);
    atc      = new ITEAudioController(itc);
    atc->setAutoFetchMetadata(true);

    auto musicDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);

    auto         nameFilters = QStringList() << "*.aac" << "*.flac" << "*.mp3" << "*.ogg" << "*.webm";
    QDirIterator it(musicDir, nameFilters, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    QStringList  files;
    int          count = 2000;
    while (it.hasNext() && count--)
        files << it.next();

    std::srand(unsigned(std::time(nullptr)));
    std::random_shuffle(files.begin(), files.end());

    while (files.size()) {
        QString   fileToPlay = files.takeLast();
        QFileInfo fi(fileToPlay);
        auto      url = QUrl::fromLocalFile(fileToPlay);
        ui->textEdit->append(fi.fileName() + "<br>");
        atc->insert(url);
    }
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::recordMic()
{
    if (!recorder) {
        recorder = new AudioRecorder(this);

        connect(recorder, &AudioRecorder::stateChanged, this, [this]() {
            if (recorder->recorder()->state() == QAudioRecorder::StoppedState) {
                recordAction->setIcon(QIcon(":/icon/recorder-microphone.png"));
            }
            if (recorder->recorder()->state() == QAudioRecorder::RecordingState) {
                recordAction->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
            }
        });

        connect(recorder, &AudioRecorder::recorded, this, [this]() {
            if (recorder->maxVolume() / double(std::numeric_limits<decltype(recorder->maxVolume())>::max()) > 0.1) {
                auto fileName = QFileInfo(recorder->recorder()->outputLocation().toLocalFile()).absoluteFilePath();
                qDebug("file=%s", qPrintable(fileName));
                atc->insert(QUrl::fromLocalFile(fileName));
            } else {
                ui->textEdit->append("Prefer silence?");
            }
        });
    }

    if (recorder->recorder()->state() == QAudioRecorder::StoppedState) {
        recorder->record(QString("test-%1.ogg").arg(QDateTime::currentSecsSinceEpoch()));
    } else {
        recorder->stop();
    }
}
