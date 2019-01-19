INCLUDEPATH *= $$PWD/../..
DEPENDPATH *= $$PWD/../..

SOURCES += $$PWD/blake2qt.cpp \
    $$PWD/blake2s-ref.c \
    $$PWD/blake2b-ref.c

HEADERS += $$PWD/blake2qt.h \
    $$PWD/blake2.h

OTHER_FILES += $$PWD/README.md
