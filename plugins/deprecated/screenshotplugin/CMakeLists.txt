unset(_HDRS)
unset(_UIS)
unset(_SRCS)
unset(_RSCS)
unset(PLUGIN)

set( PLUGIN screenshotplugin )
project(${PLUGIN} LANGUAGES CXX)
cmake_minimum_required(VERSION 3.10.0)
if(POLICY CMP0071)
    cmake_policy(SET CMP0071 NEW)
endif()
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()
set( CMAKE_AUTOMOC TRUE )

get_filename_component(ABS_PLUGINS_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PLUGINS_ROOT_DIR "${ABS_PLUGINS_ROOT_DIR}" CACHE STRING "Plugins root path. Path where include directory placed")
set( CMAKE_MODULE_PATH
    ${CMAKE_MODULE_PATH}
    ${PLUGINS_ROOT_DIR}/cmake/modules
)
find_package(PsiPluginsApi REQUIRED)
include(${PsiPluginsApi_DIR}/variables.cmake)
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${PsiPluginsApi_INCLUDE_DIR}
)
if(NOT PLUGINS_INSTALL_PATH)
    set(PLUGINS_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/${PLUGINS_PATH}")
endif()

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/qxt/core
    ${CMAKE_CURRENT_LIST_DIR}/qxt/gui
)
if( UNIX AND NOT( APPLE OR CYGWIN OR HAIKU ) )
    find_package( X11 REQUIRED )
    set(EXTRA_LIBS ${X11_LIBRARIES})
    add_definitions( -DX11 )
    set( qxt_X11_SRCS
        qxt/gui/qxtwindowsystem_x11.cpp
    )
    set( qxt_X11_HDRS
        qxt/gui/x11info.h
    )
endif()
if( APPLE )
    set( qxt_mac_SRCS
        qxt/gui/qxtwindowsystem_mac.cpp
    )
    set( qxt_mac_HRDS
        qxt/gui/qxtwindowsystem_mac.h
    )
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -framework Carbon -framework CoreFoundation")
endif()
if( WIN32 )
    add_definitions( -DQ_WS_WIN )
    set( qxt_win_SRCS
        qxt/gui/qxtwindowsystem_win.cpp
    )
endif()
set ( _HDRS
    screenshot.h
    server.h
    editserverdlg.h
    screenshotoptions.h
    toolbar.h
    pixmapwidget.h
    options.h
    optionsdlg.h
    optionswidget.h
    screenshoticonset.h
    controller.h
    defines.h
    proxysettingsdlg.h
    qxt/core/qxtglobal.h
    qxt/gui/qxtwindowsystem.h
    ${qxt_X11_HDRS}
    ${qxt_mac_HRDS}
)
set ( _SRCS
    ${PLUGIN}.cpp
    screenshot.cpp
    server.cpp
    editserverdlg.cpp
    screenshotoptions.cpp
    toolbar.cpp
    pixmapwidget.cpp
    options.cpp
    optionsdlg.cpp
    optionswidget.cpp
    screenshoticonset.cpp
    controller.cpp
    proxysettingsdlg.cpp
    qxt/core/qxtglobal.cpp
    qxt/gui/qxtwindowsystem.cpp
    ${qxt_X11_SRCS}
    ${qxt_mac_SRCS}
    ${qxt_win_SRCS}
)
set ( _UIS
    optionswidget.ui
    editserverdlg.ui
    screenshot.ui
    screenshotoptions.ui
    optionsdlg.ui
    proxysettingsdlg.ui
)
set ( _RSCS
    ${PLUGIN}.qrc
)

find_package( Qt${QT_DEFAULT_MAJOR_VERSION} COMPONENTS Widgets Xml Network PrintSupport REQUIRED )
set(QT_DEPLIBS
    Qt${QT_DEFAULT_MAJOR_VERSION}::Widgets
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network
    Qt${QT_DEFAULT_MAJOR_VERSION}::PrintSupport
)
if( UNIX AND NOT( APPLE OR CYGWIN OR HAIKU ) )
    if(QT_DEFAULT_MAJOR_VERSION GREATER_EQUAL 6)
        find_package( Qt6 COMPONENTS Gui REQUIRED)
        list(APPEND QT_DEPLIBS Qt6::Gui)
    else()
        find_package( Qt5 COMPONENTS X11Extras REQUIRED )
        list(APPEND QT_DEPLIBS Qt5::X11Extras)
    endif()
endif()
qt_wrap_ui(UIS ${_UIS})
qt_add_resources(RSCS ${_RSCS})

if(WIN32)
    set(LIB_TYPE "MODULE")
else()
    set(LIB_TYPE "SHARED")
endif()

add_library(
    ${PLUGIN}
    ${LIB_TYPE}
    ${_SRCS}
    ${UIS}
    ${RSCS}
)

target_link_libraries(
    ${PLUGIN}
    ${QT_DEPLIBS}
    ${EXTRA_LIBS}
)

install(
    TARGETS
    ${PLUGIN}
    LIBRARY
    DESTINATION
    ${PLUGINS_INSTALL_PATH}
    RUNTIME DESTINATION
    ${PLUGINS_INSTALL_PATH}
)
