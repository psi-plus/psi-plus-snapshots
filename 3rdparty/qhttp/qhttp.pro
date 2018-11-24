TEMPLATE = subdirs

APPNAME=qhttp
SUBDIRS += src
SUBDIRS += example

example.depends = src

OTHER_FILES += \
    build.properties \
    build.xml \
    qompoter.json \
    qompoter.pri \
    README.md \
    changelogs.md \

include($$PWD/vendor/qompote.pri)
$$setBuildDir()
message("$$APPNAME [ build folder is $$OBJECTS_DIR ]")
