include($$PWD/idle/idle.pri)
include($$PWD/systemwatch/systemwatch.pri)
include($$PWD/zip/zip.pri)
include($$PWD/globalshortcut/globalshortcut.pri)
include($$PWD/spellchecker/spellchecker.pri)
include($$PWD/atomicxmlfile/atomicxmlfile.pri)
include($$PWD/simplecli/simplecli.pri)

mac {
	# Growl
	contains(DEFINES, HAVE_GROWL) {
		include($$PWD/growlnotifier/growlnotifier.pri)
	}

	# Mac dock
	include($$PWD/mac_dock/mac_dock.pri)
}

HEADERS += \
	$$PWD/maybe.h \
	$$PWD/priorityvalidator.h \
	$$PWD/iodeviceopener.h

SOURCES += \
	$$PWD/priorityvalidator.cpp \
	$$PWD/iodeviceopener.cpp
