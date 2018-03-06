QT          += core network
QT          -= gui
CONFIG      += console
osx:CONFIG  -= app_bundle

TARGET       = postcollector
TEMPLATE     = app

PRJDIR       = ../..
include($$PRJDIR/commondir.pri)

HEADERS   +=

SOURCES   += main.cpp

include($$PWD/../../vendor/qompote.pri)
LIBS      += -L$${OUT_PWD}/../../src -l$$getLibName(qhttp, "Qt")
