IRIS_BASE = ../..
include(../../confapp.pri)

CONFIG -= app_bundle
QT += xml network widgets

include(../../iris.pri)

SOURCES += xmpptest.cpp
INTERFACES += test.ui
