IRIS_BASE = ../..
include(../../confapp.pri)

CONFIG += console crypto
CONFIG -= app_bundle
QT -= gui
QT += network

CONFIG *= depend_prl

INCLUDEPATH += ../../include ../../include/iris

iris_bundle:{
    include(../../src/irisnet/noncore/noncore.pri)
}
else {
    LIBS += -L$$IRIS_BASE/lib -lirisnet
}

SOURCES += main.cpp
