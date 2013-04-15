INCLUDEPATH *= $$PWD/../..
DEPENDPATH *= $$PWD/../..

HEADERS += \
	$$PWD/plainmessage.h \
	$$PWD/digestmd5proplist.h \
	$$PWD/digestmd5response.h \
	$$PWD/scramsha1message.h \
	$$PWD/scramsha1response.h \
	$$PWD/scramsha1signature.cpp

SOURCES += \
	$$PWD/plainmessage.cpp \
	$$PWD/digestmd5proplist.cpp \
	$$PWD/digestmd5response.cpp \
	$$PWD/scramsha1message.cpp \
	$$PWD/scramsha1response.cpp \
	$$PWD/scramsha1signature.cpp
