TEMPLATE = lib
TARGET = stringprep
DESTDIR  = $$IRIS_BASE/lib
CONFIG  += staticlib create_prl
OBJECTS_DIR = .obj
MOC_DIR = .moc
UI_DIR = .ui
include($$PWD/stringprep.pri)
