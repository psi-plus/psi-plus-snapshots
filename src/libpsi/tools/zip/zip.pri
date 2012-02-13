HEADERS += \
	$$PWD/zip.h

SOURCES += \
	$$PWD/zip.cpp

psi-minizip {
	SOURCES += $$PWD/minizip/unzip.c
	DEFINES += PSIMINIZIP
}

DEPENDPATH  += $$PWD

psi-winzlib {
	INCLUDEPATH += $$PWD/minizip/win32
	DEPENDPATH  += $$PWD/minizip/win32
	LIBS += $$PWD/minizip/win32/libz.a
}
mac {
	QMAKE_LFLAGS += -lz
}
