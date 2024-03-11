cmake_minimum_required(VERSION 3.10.0)

project(iris
    DESCRIPTION "XMPP network library"
    LANGUAGES C CXX
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Set 'd' prefix for Windows OS debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(WIN32)
        set(D "d")
    elseif(APPLE)
        set(D "_debug")
    endif()
endif()

# Check if Iris is a subproject
get_directory_property(IS_SUBPROJECT PARENT_DIRECTORY)

set(IRIS_LIB_VERSION_MAJOR 1)
set(IRIS_LIB_VERSION_MINOR 0)
set(IRIS_LIB_VERSION_PATCH 0)
set(IRIS_LIB_VERSION_STRING
    ${IRIS_LIB_VERSION_MAJOR}.${IRIS_LIB_VERSION_MINOR}.${IRIS_LIB_VERSION_PATCH}
)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${CMAKE_SOURCE_DIR}/cmake/modules"
)

include(policyRules)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 11)

# Detect MXE cross-compilation
set(IRIS_DEFAULT_BUNDLED_USRSCTP OFF)
if(NOT DEFINED USE_MXE AND (EXISTS "${CMAKE_TOOLCHAIN_FILE}"))
    option(USE_MXE "Use MXE (cross-compilation build environment for MS Windows)" OFF)
    string(TOLOWER ${CMAKE_TOOLCHAIN_FILE} TOOLCHAIN_FILE)
    string(REGEX MATCH "mxe-conf" MXE_DETECTED "${TOOLCHAIN_FILE}")
    if(MXE_DETECTED)
        message(STATUS "MXE environment detected")
        message(STATUS "MXE toolchain: ${CMAKE_TOOLCHAIN_FILE}")
        message(STATUS "MXE root path: ${CMAKE_PREFIX_PATH}")
        set(USE_MXE ON)
        set(IRIS_DEFAULT_BUNDLED_USRSCTP ON)
    endif()
endif()

set(IRIS_DEFAULT_BUNDLED_QCA OFF)
if(APPLE OR (MSVC OR USE_MXE))
    set(IRIS_DEFAULT_BUNDLED_QCA ON)
endif()

option(IRIS_ENABLE_INSTALL "Enable installation" ON)
option(IRIS_ENABLE_JINGLE_SCTP "Enable SCTP over ICE Jingle transport / data channels" ON)
option(IRIS_BUNDLED_QCA "Adds: DTLS, Blake2b and other useful for XMPP crypto-stuff" ${IRIS_DEFAULT_BUNDLED_QCA})
option(IRIS_BUNDLED_USRSCTP "Compile compatible UsrSCTP lib (required for datachannel Jingle transport)" ${IRIS_DEFAULT_BUNDLED_USRSCTP})
option(IRIS_BUILD_TOOLS "Build tools and examples" OFF)
option(IRIS_ENABLE_DEBUG "Enable debugging code paths" OFF)

set(IRIS_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR}/xmpp/iris)

set(CMAKE_CXX_STANDARD 17)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo"))
    include(debug-definitions)
endif()

find_package(Qt5 COMPONENTS Core Gui Xml Network REQUIRED)

if(Qt5Core_FOUND)
    message(STATUS "Qt5 found, version ${Qt5Core_VERSION}")
    if(IRIS_BUNDLED_QCA AND ("${Qt5Core_VERSION}" VERSION_LESS "5.6"))
        message(FATAL_ERROR "Minimal Qt version for bundled Qca is 5.6. Please disable IRIS_BUNDLED_QCA option")
    endif()
endif()

# Find QCA package or use bundled if enabled
include(IrisQCA)

set(STDINT_FOUND OFF)
find_file(STDINT_INCLUDE stdint.h)

if(NOT ${STDINT_INCLUDE} STREQUAL "STDINT_INCLUDE-NOTFOUND")
    message(STATUS "StdInt include found: ${STDINT_INCLUDE}")
    set(STDINT_FOUND ON)
endif()

# Find SCTP package or use bundled if enabled
if(IRIS_ENABLE_JINGLE_SCTP)
    include(IrisSCTP)
endif()

if(NOT IRIS_BUNDLED_QCA)
    find_package(B2 QUIET)
    if(B2_FOUND)
        message(STATUS "Found B2: ${B2_LIBRARY}")
    endif()
endif()

add_subdirectory(3rdparty/stringprep)
add_subdirectory(src/irisnet)
add_subdirectory(src/xmpp)

if(IRIS_BUILD_TOOLS)
    if(NOT IRIS_BUNDLED_QCA)
        message(FATAL_ERROR "Bundled Qca is needed to build tools")
    endif()
    add_subdirectory(tools)
endif()

if(NOT IS_SUBPROJECT)
    include(fix-codestyle)
endif()

if(IRIS_ENABLE_INSTALL)
    include(CMakePackageConfigHelpers)
    include(GNUInstallDirs)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/IrisConfigVersion.cmake
        VERSION ${IRIS_LIB_VERSION_STRING}
        COMPATIBILITY SameMajorVersion
    )
    configure_file(
        iris.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/iris.pc
        @ONLY
    )

    install(TARGETS iris
        EXPORT iris
        LIBRARY DESTINATION  ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION  ${CMAKE_INSTALL_LIBDIR}
    )
    install(EXPORT iris
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
        NAMESPACE iris::
        FILE IrisConfig.cmake
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/IrisConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Iris
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/iris.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    install(DIRECTORY include/iris
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xmpp
        FILES_MATCHING PATTERN "*.h"
    )
endif()
