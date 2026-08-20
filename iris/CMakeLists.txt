cmake_minimum_required(VERSION 3.11.0)

project(iris
    VERSION 1.0.0
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

# Standalone Iris is a shared library by default. When Iris is embedded as a
# subproject, src/xmpp/CMakeLists.txt keeps using an explicit STATIC target.
if(NOT IS_SUBPROJECT)
    option(BUILD_SHARED_LIBS "Build Iris as a shared library" ON)
endif()
option(IRIS_POSITION_INDEPENDENT_CODE
    "Build static Iris and its internal libraries as position-independent code" OFF)

set(IRIS_LIB_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(IRIS_LIB_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(IRIS_LIB_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(IRIS_LIB_VERSION_STRING ${PROJECT_VERSION})

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${CMAKE_SOURCE_DIR}/cmake/modules"
)

include(policyRules)
include(GNUInstallDirs)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 20)

# irisnet, stringprep and other internal static libraries must be PIC when
# linked into a shared Iris or when an embedding project explicitly requests
# a PIC-capable static Iris (for example, to link it into a shared plugin).
if(BUILD_SHARED_LIBS OR IRIS_POSITION_INDEPENDENT_CODE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Detect MXE cross-compilation
set(IRIS_DEFAULT_BUNDLED_USRSCTP OFF)
if(ANDROID)
    set(IRIS_DEFAULT_BUNDLED_USRSCTP ON)
endif()

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
if(APPLE OR ANDROID OR (MSVC OR USE_MXE))
    set(IRIS_DEFAULT_BUNDLED_QCA ON)
endif()

option(IRIS_ENABLE_INSTALL "Enable installation" ON)
option(IRIS_ENABLE_JINGLE_SCTP "Enable SCTP over ICE Jingle transport / data channels" ON)
# note Blake2b is needed only with Qt5. Qt6 has its own implementation
option(IRIS_BUNDLED_QCA "Adds: DTLS, Blake2b (needed with Qt5) and other useful for XMPP crypto-stuff" ${IRIS_DEFAULT_BUNDLED_QCA})
option(IRIS_BUNDLED_USRSCTP "Compile compatible UsrSCTP lib (required for datachannel Jingle transport)" ${IRIS_DEFAULT_BUNDLED_USRSCTP})
option(IRIS_ENABLE_OMEMO "Enable XEP-0384 OMEMO support (requires GPLv3 libomemo-c)" OFF)
option(IRIS_BUNDLED_OMEMO_C "Build libomemo-c and protobuf-c when OMEMO is enabled" ON)
option(IRIS_BUILD_TOOLS "Build tools and examples" OFF)
option(IRIS_ENABLE_DEBUG "Enable debugging code paths" OFF)
option(USE_QT6 "Enable Qt6 support" OFF)

# Optional installation namespace. Use CamelCase here (for example Xmpp);
# filesystem/library names are lower-cased while the CMake package directory
# remains CamelCase (IrisXmpp), so find_package(Iris CONFIG) can still discover
# it through CMake's standard Iris* package search layout.
set(IRIS_INSTALL_NAMESPACE "" CACHE STRING "Optional Iris install namespace, e.g. Xmpp")
if(IRIS_INSTALL_NAMESPACE)
    string(TOLOWER "${IRIS_INSTALL_NAMESPACE}" IRIS_INSTALL_NAMESPACE_LOWER)
    set(IRIS_NAME_PREFIX "${IRIS_INSTALL_NAMESPACE_LOWER}-")
    set(IRIS_INSTALL_INCLUDE_ROOT "${CMAKE_INSTALL_INCLUDEDIR}/${IRIS_INSTALL_NAMESPACE_LOWER}")
    set(IRIS_CMAKE_PACKAGE_DIR "Iris${IRIS_INSTALL_NAMESPACE}")
else()
    set(IRIS_INSTALL_NAMESPACE_LOWER "")
    set(IRIS_NAME_PREFIX "")
    set(IRIS_INSTALL_INCLUDE_ROOT "${CMAKE_INSTALL_INCLUDEDIR}")
    set(IRIS_CMAKE_PACKAGE_DIR "Iris")
endif()
set(IRIS_INSTALL_INCLUDEDIR "${IRIS_INSTALL_INCLUDE_ROOT}/iris")

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo"))
    include(debug-definitions)
endif()

if(USE_QT6)
    set(QT_DEFAULT_MAJOR_VERSION 6)
endif()
if("${QT_DEFAULT_MAJOR_VERSION}" STREQUAL "")
    set(QT_DEFAULT_MAJOR_VERSION 5)
endif()

if(QT_DEFAULT_MAJOR_VERSION LESS 6)
    find_package(Qt5 5.10 REQUIRED COMPONENTS Core Gui Xml Network)
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Xml Network)
endif()
message(STATUS "Qt${QT_DEFAULT_MAJOR_VERSION} found, version ${Qt${QT_DEFAULT_MAJOR_VERSION}Core_VERSION}")
set(IRIS_LIB_NAME "${IRIS_NAME_PREFIX}iris-qt${QT_DEFAULT_MAJOR_VERSION}")

if(IS_SUBPROJECT)
    set(IRIS_ENABLE_INSTALL OFF)
endif()

# Find QCA package or use bundled if enabled
include(IrisQCA)
include(IrisQCAFeatures)

if(IRIS_BUNDLED_QCA)
    set(IRIS_BUNDLED_QCA_INCLUDE_REL
        "${IRIS_INSTALL_INCLUDEDIR}/qca/Qca3-qt${QT_DEFAULT_MAJOR_VERSION}/QtCrypto")
    set(IRIS_PC_QCA_REQUIRES "")
    set(IRIS_PC_QCA_CFLAGS
        " -I\${irisincludedir}/qca/Qca3-qt${QT_DEFAULT_MAJOR_VERSION}/QtCrypto")
elseif(IRIS_QCA_MAJOR EQUAL 3)
    set(IRIS_BUNDLED_QCA_INCLUDE_REL "")
    set(IRIS_PC_QCA_REQUIRES " qca3-qt${QT_DEFAULT_MAJOR_VERSION}")
    set(IRIS_PC_QCA_CFLAGS "")
else()
    set(IRIS_BUNDLED_QCA_INCLUDE_REL "")
    set(IRIS_PC_QCA_REQUIRES " qca2-qt${QT_DEFAULT_MAJOR_VERSION}")
    set(IRIS_PC_QCA_CFLAGS "")
endif()

set(STDINT_FOUND OFF)
find_file(STDINT_INCLUDE stdint.h)

if(NOT ${STDINT_INCLUDE} STREQUAL "STDINT_INCLUDE-NOTFOUND")
    message(STATUS "StdInt include found: ${STDINT_INCLUDE}")
    set(STDINT_FOUND ON)
endif()

# Find OMEMO Double Ratchet backend only for builds that explicitly enable it.
if(IRIS_ENABLE_OMEMO)
    include(IrisOMEMO)
endif()

# Find SCTP package or use bundled if enabled
if(IRIS_ENABLE_JINGLE_SCTP)
    include(IrisSCTP)
endif()

if(NOT IRIS_BUNDLED_QCA AND QT_DEFAULT_MAJOR_VERSION LESS 6)
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
        ${CMAKE_CURRENT_BINARY_DIR}/${IRIS_LIB_NAME}.pc
        @ONLY
    )
    set(CONFIG_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/${IRIS_CMAKE_PACKAGE_DIR})
    install(TARGETS iris
        EXPORT IrisTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(EXPORT IrisTargets
        DESTINATION ${CONFIG_INSTALL_DIR}
        NAMESPACE Iris::
        FILE IrisTargets.cmake
    )
    if(IRIS_BUNDLED_QCA)
        # QCA is linked statically into a shared Iris. Do not install a second
        # global QCA library/provider set: that would collide with a system
        # QCA installation and could create two independent QCA runtimes.
        # Public Iris headers expose QCA types, so install only the headers in
        # an Iris-private SDK subtree. IrisConfig.cmake recreates Qca3::Qca as
        # an interface target backed by Iris::Iris.
        if(NOT BUILD_SHARED_LIBS)
            message(FATAL_ERROR
                "Installing Iris with bundled QCA currently requires BUILD_SHARED_LIBS=ON")
        endif()

        install(DIRECTORY
            "${IRIS_QCA_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}/Qca3-qt${QT_DEFAULT_MAJOR_VERSION}/QtCrypto/"
            DESTINATION "${IRIS_BUNDLED_QCA_INCLUDE_REL}"
        )
    endif()
    # Bundled usrsctp is a private implementation detail. For shared Iris it
    # is linked statically into libiris and must not install a separate
    # usrsctp archive/header set into the user's prefix. Iris public headers
    # do not expose usrsctp types.
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/IrisConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/IrisConfig.cmake"
        INSTALL_DESTINATION ${CONFIG_INSTALL_DIR}
    )
    if(NOT IRIS_BUNDLED_QCA AND IRIS_QCA_MAJOR EQUAL 2)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/FindQca.cmake"
            DESTINATION "${CONFIG_INSTALL_DIR}/modules")
    endif()
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/IrisConfigVersion.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/IrisConfig.cmake"
        DESTINATION ${CONFIG_INSTALL_DIR}
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${IRIS_LIB_NAME}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    # Keep the historical flat include facade for source compatibility.
    # Canonical public headers are installed by src/xmpp and src/irisnet into
    # ${IRIS_INSTALL_INCLUDEDIR}/{xmpp-core,xmpp-im,jid,irisnet/...}.
    file(GLOB IRIS_COMPAT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/iris/*.h")
    list(REMOVE_ITEM IRIS_COMPAT_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/iris/xmpp_omemo.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/iris/jingle-sctp.h"
    )
    install(FILES ${IRIS_COMPAT_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}
    )
    if(IRIS_ENABLE_OMEMO)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/iris/xmpp_omemo.h"
            DESTINATION ${IRIS_INSTALL_INCLUDEDIR}
        )
    endif()
    if(IRIS_ENABLE_JINGLE_SCTP)
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/iris/jingle-sctp.h"
            DESTINATION ${IRIS_INSTALL_INCLUDEDIR}
        )
    endif()
endif()
