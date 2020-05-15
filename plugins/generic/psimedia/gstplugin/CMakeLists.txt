unset(_HDRS)
unset(_UIS)
unset(_SRCS)
unset(_RSCS)
unset(PLUGIN)

set( PLUGIN gstprovider )
project(${PLUGIN} LANGUAGES CXX)
cmake_minimum_required(VERSION 3.1.0)
if(POLICY CMP0071)
    cmake_policy(SET CMP0071 OLD)
endif()
find_package(Qt5 COMPONENTS Core REQUIRED)
set( CMAKE_AUTOMOC TRUE )

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
)

set(SOURCES
    gstplugin.cpp
)

if(WIN32)
    set(LIB_TYPE "MODULE")
else()
    set(LIB_TYPE "SHARED")
endif()

add_library(
    ${PLUGIN}
    ${LIB_TYPE}
    ${SOURCES}
)
target_link_libraries(
    ${PLUGIN}
    ${QT_DEPLIBS}
)
target_link_libraries(${PLUGIN} gstprovidersrc)
install(TARGETS ${PLUGIN} LIBRARY DESTINATION ${LIB_INSTALL_DIR})
