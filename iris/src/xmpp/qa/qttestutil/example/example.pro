#
# Example unit test module with 2 unit test suites.
#

include(../qttestutil.pri)

QT += testlib
QT -= gui
CONFIG -= app_bundle

TARGET = checker

SOURCES += \
	myfirstclasstest.cpp \
	mysecondclasstest.cpp \
	../simplechecker.cpp

# Add an extra 'make check' target.
QMAKE_EXTRA_TARGETS = check
check.commands = \$(MAKE) && ./$(QMAKE_TARGET)

# Cleanup the checker on 'make clean'
QMAKE_CLEAN += $(QMAKE_TARGET)
