if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32)
    set(D "d")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND APPLE)
    set(D "_debug")
endif()

get_directory_property(IS_SUBPROJECT PARENT_DIRECTORY)

project(iris)
cmake_minimum_required(VERSION 3.2.0)

#Set automoc and autouic policy
if(POLICY CMP0071)
    if(${CMAKE_VERSION} VERSION_LESS "3.10.0")
        cmake_policy(SET CMP0071 OLD)
        message(STATUS "CMP0071 policy set to OLD")
    else()
        cmake_policy(SET CMP0071 NEW)
        message(STATUS "CMP0071 policy set to NEW")
    endif()
endif()
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
    message(STATUS "CMP0074 policy set to NEW")
endif()

set( IRIS_LIB_VERSION_MAJOR 1 )
set( IRIS_LIB_VERSION_MINOR 0 )
set( IRIS_LIB_VERSION_PATCH 0 )
set( IRIS_LIB_VERSION_STRING
    ${IRIS_LIB_VERSION_MAJOR}.${IRIS_LIB_VERSION_MINOR}.${IRIS_LIB_VERSION_PATCH}
    )

get_filename_component(ABS_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set( CMAKE_MODULE_PATH
    "${CMAKE_MODULE_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${ABS_PARENT_DIR}/cmake/modules"
    "${PROJECT_SOURCE_DIR}/cmake/modules"
    )

set( DEFAULT_BUNDLED_QCA OFF )
if (APPLE OR MSVC)
    set( DEFAULT_BUNDLED_QCA ON )
endif()

option( JINGLE_SCTP "Enable SCTP over ICE Jingle transport / data channels" ON )
option( BUNDLED_QCA "Adds: DTLS, Blake2b and other useful for XMPP crypto-stuff" ${DEFAULT_BUNDLED_QCA})
option( BUILD_IRIS_TOOLS "Build tools and examples" OFF)

set(CMAKE_CXX_STANDARD 17)

include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})
include_directories(include/iris)

set(CMAKE_AUTOMOC ON)

find_package(Qt5 COMPONENTS Core Gui Xml Network REQUIRED)

if(Qt5Core_FOUND)
    message(STATUS "Qt5 found, version ${Qt5Core_VERSION}")
    if(BUNDLED_QCA AND("${Qt5Core_VERSION}" VERSION_LESS "5.6"))
        message(FATAL_ERROR "Minimal Qt version for bundled Qca is 5.6. Please disable BUNDLED_QCA option")
    endif()
endif()

if(BUNDLED_QCA)
    message(STATUS "QCA: using bundled")
    set(QCA_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/qca)
    set(QCA_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/qca)
    set(QCA_BUILD_DIR ${QCA_PREFIX}/build)
    set(Qca_INCLUDE_DIR ${QCA_BUILD_DIR})
    if(NOT EXISTS ${QCA_SOURCE_DIR})
        list(APPEND Qca_INCLUDE_DIR ${QCA_PREFIX}/src/QcaProject/include/QtCrypto)
    else()
        list(APPEND Qca_INCLUDE_DIR ${QCA_SOURCE_DIR}/include/QtCrypto)
    endif()
    set(Qca_CORE_LIB ${QCA_BUILD_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}qca-qt5${D}${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(Qca_OSSL_LIB ${QCA_BUILD_DIR}/lib/qca-qt5/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl${D}${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(Qca_LIBRARY ${Qca_OSSL_LIB} ${Qca_CORE_LIB})
    if(APPLE)
        set(COREFOUNDATION_LIBRARY "-framework CoreFoundation")
        set(COREFOUNDATION_LIBRARY_SECURITY "-framework Security")
        set(Qca_LIBRARY ${Qca_LIBRARY} ${COREFOUNDATION_LIBRARY} ${COREFOUNDATION_LIBRARY_SECURITY})
    endif()
    if(IS_SUBPROJECT)
        set(Qca_LIBRARY_EXPORT ${Qca_LIBRARY} PARENT_SCOPE)
        set(Qca_INCLUDE_DIR_EXPORT ${Qca_INCLUDE_DIR} PARENT_SCOPE)
    endif()

    if ("${OPENSSL_ROOT_DIR}" STREQUAL "" AND APPLE)
        set(OPENSSL_ROOT_DIR /usr/local/opt/openssl)
    endif()
    find_package(OpenSSL REQUIRED)

    include(ExternalProject)
    #set CMake options and transfer the environment to an external project
    set(QCA_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF -DBUILD_PLUGINS=ossl -DLOAD_SHARED_PLUGINS=OFF
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${QCA_PREFIX} -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DOSX_FRAMEWORK=OFF)
    if (EXISTS ${QCA_SOURCE_DIR})
        message(STATUS "QCA: found bundled sources")
        ExternalProject_Add(QcaProject
            PREFIX ${QCA_PREFIX}
            BINARY_DIR ${QCA_BUILD_DIR}
            SOURCE_DIR ${QCA_SOURCE_DIR}
            CMAKE_ARGS ${QCA_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${Qca_LIBRARY}
            INSTALL_COMMAND ""
            )
    else()
        include(FindGit)
        find_package(Git)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "Git not found! Bundled Qca needs Git utility.\nPlease set GIT_EXECUTABLE variable or add git to PATH")
        endif()
        ExternalProject_Add(QcaProject
            PREFIX ${QCA_PREFIX}
            BINARY_DIR ${QCA_BUILD_DIR}
            GIT_REPOSITORY https://github.com/psi-im/qca.git
            CMAKE_ARGS ${QCA_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${Qca_LIBRARY}
            INSTALL_COMMAND ""
            )
    endif()
else()
    message(WARNING "Disabling BUNDLED_QCA option makes impossible to use DTLS and PsiMedia")
    message(STATUS "QCA: using system")
    find_package(Qca REQUIRED)
endif()

set(QCA_INCLUDES ${Qca_INCLUDE_DIR})
set(qca_LIB ${Qca_LIBRARY})

include_directories(
    ${Qca_INCLUDE_DIR}
)

set(STDINT_FOUND OFF)
find_file(STDINT_INCLUDE stdint.h)

if(NOT ${STDINT_INCLUDE} STREQUAL "STDINT_INCLUDE-NOTFOUND")
    message(STATUS "StdInt include found: ${STDINT_INCLUDE}")
    set(STDINT_FOUND ON)
endif()

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
    include(cmake/modules/fix-codestyle.cmake)
endif()
