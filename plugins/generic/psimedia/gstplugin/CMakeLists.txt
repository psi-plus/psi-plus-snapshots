unset(_HDRS)
unset(_UIS)
unset(_SRCS)
unset(_RSCS)
unset(PLUGIN)

cmake_minimum_required(VERSION 3.10.0)

set( PLUGIN gstprovider )
project(${PLUGIN} LANGUAGES CXX)
if(POLICY CMP0071)
    cmake_policy(SET CMP0071 NEW)
endif()
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} COMPONENTS Core REQUIRED)
set( CMAKE_AUTOMOC TRUE )

get_filename_component(ABS_GPLUG_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${ABS_GPLUG_PARENT_DIR}/psimedia
    ${ABS_GPLUG_PARENT_DIR}/gstprovider
    ${ABS_GPLUG_PARENT_DIR}/gstprovider/devices
)

set(SOURCES
    gstplugin.cpp
)

if(WIN32)
    set(LIB_TYPE "MODULE")
else()
    set(LIB_TYPE "SHARED")
endif()
list(APPEND QT_DEPLIBS Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_library(
    ${PLUGIN}
    ${LIB_TYPE}
    ${SOURCES}
)
target_link_libraries(
    ${PLUGIN}
    ${QT_DEPLIBS}
)
add_dependencies(${PLUGIN} gstprovidersrc)
target_link_libraries(${PLUGIN} gstprovidersrc)
install(TARGETS ${PLUGIN} LIBRARY DESTINATION ${LIB_INSTALL_DIR})

