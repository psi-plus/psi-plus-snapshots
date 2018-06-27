HEADERS += \
    $$PWD/zip.h

SOURCES += \
    $$PWD/zip.cpp

psi-minizip {
    SOURCES += $$PWD/minizip/unzip.c $$PWD/minizip/ioapi.c
    DEFINES += PSIMINIZIP
}

DEPENDPATH  += $$PWD

mac {
    QMAKE_LFLAGS += -lz
}
