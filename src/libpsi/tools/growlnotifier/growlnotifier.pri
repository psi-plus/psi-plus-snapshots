DEPENDPATH  += $$PWD

HEADERS += $$PWD/growlnotifier.h
OBJECTIVE_SOURCES  += $$PWD/growlnotifier.mm
QMAKE_LFLAGS += -framework Growl -framework Cocoa
