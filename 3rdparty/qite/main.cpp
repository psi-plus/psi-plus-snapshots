#include "mainwindow.h"
#include <QApplication>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QAudioRecorder>
#endif

int main(int argc, char *argv[])
{

    QApplication a(argc, argv);
    Q_INIT_RESOURCE(main);
    MainWindow w;
    w.show();
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QAudioRecorder *recorder       = new QAudioRecorder();
    QStringList     codecs_list    = recorder->supportedAudioCodecs();
    QStringList     container_list = recorder->supportedContainers();

    qDebug("RECORDER CODECS:");
    for (int i = 0; i < codecs_list.count(); i++) {
        qDebug() << codecs_list[i];
    }

    qDebug("\nRECORDER CONTAINERS:");
    for (int i = 0; i < codecs_list.count(); i++) {
        qDebug() << codecs_list[i];
    }
#endif

    return a.exec();
}
