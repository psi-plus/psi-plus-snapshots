!greaterThan(QT_MAJOR_VERSION, 4):error(Qt5 or above is required)

SOURCES += \
    $$PWD/qite.cpp \
    $$PWD/qitemedia.cpp \
    $$PWD/qitevideo.cpp \
    $$PWD/qitevideoframe.cpp \
    $$PWD/qiteaudio.cpp \
    $$PWD/qiteprogress.cpp \
    $$PWD/qiteaudiorecorder.cpp

HEADERS += \
    $$PWD/qite.h \
    $$PWD/qitemedia.h \
    $$PWD/qitemedia_p.h \
    $$PWD/qitemediaplayer_p.h \
    $$PWD/qitevideo.h \
    $$PWD/qitevideoframe_p.h \
    $$PWD/qiteaudio.h \
    $$PWD/qiteprogress.h \
    $$PWD/qiteaudiorecorder.h

INCLUDEPATH += $$PWD
