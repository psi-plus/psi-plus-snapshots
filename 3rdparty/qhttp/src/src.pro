DEPENDPATH += $$PWD
INCLUDEPATH += $$PWD $$PWD/. $$PWD/..

include($$PWD/../vendor/vendor.pri)
PRJDIR       = ..
include($$PRJDIR/commondir.pri)
$$setLibPath()

VENDORNAME=oliviermaridat
APPNAME=qhttp
TARGET = $$getLibName($$APPNAME, "Qt")
TEMPLATE = lib
CONFIG += staticlib
#CONFIG += debug_and_release build_all
QT += network
QT -= gui
CONFIG += c++11
VERSION = 3.1.2

defined(EXPORT_PATH_PREFIX, "var"){
    EXPORT_PATH = $$EXPORT_PATH_PREFIX
}
else{
    EXPORT_PATH = $$OUT_PWD/export
}
EXPORT_PATH = $${EXPORT_PATH}/$${VENDORNAME}/$${APPNAME}/v$${VERSION}-lib
EXPORT_INCLUDEPATH = $$EXPORT_PATH/include
EXPORT_LIBPATH = $$EXPORT_PATH/$$LIBPATH
message("$$APPNAME [ export folder is $${EXPORT_LIBPATH} ]")

DEFINES       *= QHTTP_MEMORY_LOG=0
win32:DEFINES *= QHTTP_EXPORT

# Joyent http_parser
SOURCES  += $$PWD/../vendor/http-parser/http_parser.c
HEADERS  += $$PWD/../vendor/http-parser/http_parser.h

SOURCES  += \
    qhttpabstracts.cpp \
    qhttpserverconnection.cpp \
    qhttpserverrequest.cpp \
    qhttpserverresponse.cpp \
    qhttpserver.cpp

PUBLIC_HEADERS  += \
    qhttpfwd.hpp \
    qhttpabstracts.hpp \
    qhttpserverconnection.hpp \
    qhttpserverrequest.hpp \
    qhttpserverresponse.hpp \
    qhttpserver.hpp

contains(DEFINES, QHTTP_HAS_CLIENT) {
    SOURCES += \
        qhttpclientrequest.cpp \
        qhttpclientresponse.cpp \
        qhttpclient.cpp

    PUBLIC_HEADERS += \
        qhttpclient.hpp \
        qhttpclientresponse.hpp \
        qhttpclientrequest.hpp
}


HEADERS += $${PUBLIC_HEADERS}

# Lib
QMAKE_STRIP = echo # Avoid striping header files (which will not work)
target.extra = strip $(TARGET)
applibs.files = $${DESTDIR}/*.a
applibs.path = $$EXPORT_LIBPATH
INSTALLS += applibs

# Include files
headers.files = $$PUBLIC_HEADERS
headers.path = $$EXPORT_INCLUDEPATH
INSTALLS += headers

## qompoter.pri
qompoter.files = $$PWD/../qompoter.pri
qompoter.files += $$PWD/../qompoter.json
qompoter.path = $$EXPORT_PATH
INSTALLS += qompoter

