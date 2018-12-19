!greaterThan(QT_MAJOR_VERSION, 4):error(Qt5 or above is required)

SOURCES += \
    $$PWD/qite.cpp \
    $$PWD/qiteaudio.cpp \
    $$PWD/qiteaudiorecorder.cpp

HEADERS += \
    $$PWD/qite.h \
    $$PWD/qiteaudio.h \
    $$PWD/qiteaudiorecorder.h

INCLUDEPATH += $$PWD
