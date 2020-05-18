INCLUDEPATH *= $$PWD/../..
DEPENDPATH *= $$PWD/../..

SOURCES += $$PWD/blake2qt.cpp

bundled_blake2 {
    SOURCES += \
        $$PWD/blake2s-ref.c \
        $$PWD/blake2b-ref.c
    HEADERS += $$PWD/blake2.h
    INCLUDEPATH += $PWD
} else {
    DEFINES += IRIS_SYSTEM_BLAKE2
}

HEADERS += $$PWD/blake2qt.h

OTHER_FILES += $$PWD/README.md
