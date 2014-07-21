IRIS_BASE = $$PWD/..

isEmpty(top_iris_builddir):top_iris_builddir = .
include($$top_iris_builddir/../conf.pri)
windows:include(../conf_win.pri)

include(../common.pri)

QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.3
