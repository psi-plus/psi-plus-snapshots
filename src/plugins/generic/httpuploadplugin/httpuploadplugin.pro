isEmpty(PSISDK) {
    include(../../psiplugin.pri)
} else {
    include($$PSISDK/plugins/psiplugin.pri)
}

QT += network
greaterThan(QT_MAJOR_VERSION, 4) {
    CONFIG += c++11
}

RESOURCES = httpuploadplugin.qrc

SOURCES += httpuploadplugin.cpp \
           uploadservice.cpp \
           previewfiledialog.cpp \

HEADERS += previewfiledialog.h
