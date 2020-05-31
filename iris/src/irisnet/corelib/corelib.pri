QT *= network

HEADERS += \
    $$PWD/objectsession.h \
    $$PWD/irisnetexport.h \
    $$PWD/irisnetplugin.h \
    $$PWD/irisnetglobal.h \
    $$PWD/irisnetglobal_p.h \
    $$PWD/netinterface.h \
    $$PWD/netavailability.h \
    $$PWD/netnames.h \
    $$PWD/addressresolver.h

SOURCES += \
    $$PWD/objectsession.cpp \
    $$PWD/irisnetplugin.cpp \
    $$PWD/irisnetglobal.cpp \
    $$PWD/netinterface.cpp \
    $$PWD/netavailability.cpp \
    $$PWD/netnames.cpp \
    $$PWD/addressresolver.cpp

unix {
    SOURCES += \
        $$PWD/netinterface_unix.cpp
}

need_jdns|lessThan(QT_MAJOR_VERSION, 5) {
    !ext-qjdns {
        include(../../jdns/jdns.pri)
        INCLUDEPATH += $$PWD/../../jdns
    }

    SOURCES += \
        $$PWD/netnames_jdns.cpp

    DEFINES += NEED_JDNS
} else {
    SOURCES += $$PWD/netinterface_qtname.cpp \
}

equals(QT_MAJOR_VERSION, 5):lessThan(QT_MINOR_VERSION, 15) {
    SOURCES += $$PWD/netinterface_qtnet.cpp
    DEFINES += HAVE_QTNET
}

#include(legacy/legacy.pri)

appledns:appledns_bundle {
    DEFINES += APPLEDNS_STATIC
    include(../appledns/appledns.pri)
}
