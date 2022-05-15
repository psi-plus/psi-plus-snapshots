cmake_minimum_required(VERSION 3.10.0)
project(iris)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

#Sed d prefix for Windows OS debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32)
    set(D "d")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND APPLE)
    set(D "_debug")
endif()

#Check if Iris is subproject
get_directory_property(IS_SUBPROJECT PARENT_DIRECTORY)

set( IRIS_LIB_VERSION_MAJOR 1 )
set( IRIS_LIB_VERSION_MINOR 0 )
set( IRIS_LIB_VERSION_PATCH 0 )
set( IRIS_LIB_VERSION_STRING
    ${IRIS_LIB_VERSION_MAJOR}.${IRIS_LIB_VERSION_MINOR}.${IRIS_LIB_VERSION_PATCH}
    )

get_filename_component(ABS_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${ABS_PARENT_DIR}/cmake/modules"
    "${PROJECT_SOURCE_DIR}/cmake/modules"
    )

include(policyRules)

set( DEFAULT_BUNDLED_QCA OFF )

set(CMAKE_AUTOMOC ON)

# Detect MXE cross-compilation
if(NOT DEFINED USE_MXE AND (EXISTS "${CMAKE_TOOLCHAIN_FILE}"))
    option( USE_MXE "Use MXE (cross-compilation build environment for MS Windows)" OFF )
    string(TOLOWER ${CMAKE_TOOLCHAIN_FILE} TOOLCHAIN_FILE)
    string(REGEX MATCH "mxe-conf" MXE_DETECTED "${TOOLCHAIN_FILE}")
    if(MXE_DETECTED)
        message(STATUS "MXE environment detected")
        message(STATUS "MXE toolchain: ${CMAKE_TOOLCHAIN_FILE}")
        message(STATUS "MXE root path: ${CMAKE_PREFIX_PATH}")
        set(USE_MXE ON)
        set(BUNDLED_USRSCTP ON)
    endif()
endif()

if (APPLE OR (MSVC OR USE_MXE))
    set( DEFAULT_BUNDLED_QCA ON )
endif()

option( JINGLE_SCTP "Enable SCTP over ICE Jingle transport / data channels" ON )
option( BUNDLED_QCA "Adds: DTLS, Blake2b and other useful for XMPP crypto-stuff" ${DEFAULT_BUNDLED_QCA})
option( BUILD_IRIS_TOOLS "Build tools and examples" OFF)

set(CMAKE_CXX_STANDARD 17)

include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})
include_directories(include/iris)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo"))
    include(debug-definitions)
endif()

find_package(Qt5 COMPONENTS Core Gui Xml Network REQUIRED)

if(Qt5Core_FOUND)
    message(STATUS "Qt5 found, version ${Qt5Core_VERSION}")
    if(BUNDLED_QCA AND("${Qt5Core_VERSION}" VERSION_LESS "5.6"))
        message(FATAL_ERROR "Minimal Qt version for bundled Qca is 5.6. Please disable BUNDLED_QCA option")
    endif()
endif()

#Find QCA package or use bundled if enabled
include(IrisQCA)

set(STDINT_FOUND OFF)

find_file(STDINT_INCLUDE stdint.h)

if(NOT ${STDINT_INCLUDE} STREQUAL "STDINT_INCLUDE-NOTFOUND")
    message(STATUS "StdInt include found: ${STDINT_INCLUDE}")
    set(STDINT_FOUND ON)
endif()

#Find SCTP package or use bundled if enabled
if(JINGLE_SCTP)
    include(IrisSCTP)
endif()


if (NOT BUNDLED_QCA)
    find_package(B2 QUIET)
    if(B2_FOUND)
        message(STATUS "Found B2: ${B2_LIBRARY}")
    endif()
endif()

add_definitions(-DIRISNET_STATIC -DQSTRINGPREP_BUILDING)

add_subdirectory(3rdparty/stringprep)
add_subdirectory(src/irisnet)
add_subdirectory(src/xmpp)
if(BUILD_IRIS_TOOLS)
    add_subdirectory(tools)
endif()
if(NOT IS_SUBPROJECT)
    include(fix-codestyle)
endif()
