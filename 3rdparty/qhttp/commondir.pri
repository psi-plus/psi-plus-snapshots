# specifying common dirs

# comment following line to build the lib as static library
DEFINES *= QHTTP_DYNAMIC_LIB
# comment following line to trim client classes from build
DEFINES *= QHTTP_HAS_CLIENT
CONFIG  += c++11

unix {
    TEMPDIR      = $$PRJDIR/tmp/unix/$$TARGET
    macx:TEMPDIR = $$PRJDIR/tmp/osx/$$TARGET
}

win32 {
    TEMPDIR  = $$PRJDIR/tmp/win32/$$TARGET
    DEFINES += _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
}

DESTDIR      = $$OUT_PWD/$$PRJDIR/xbin
MOC_DIR      = $$TEMPDIR
OBJECTS_DIR  = $$TEMPDIR
RCC_DIR      = $$TEMPDIR
UI_DIR       = $$TEMPDIR/Ui
LIBS        += -L$$OUT_PWD/$$PRJDIR/xbin

INCLUDEPATH +=  . $$PRJDIR/src

CONFIG += qhttp
include($$PWD/vendor/vendor.pri)

