unix:exists(confapp_unix.pri):include(confapp_unix.pri)
windows:exists(confapp_win.pri):include(confapp_win.pri)

include(common.pri)

mac:QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.9
