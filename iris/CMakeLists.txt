if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
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

option( USE_QJDNS "Use qjdns/jdns library. Disabled by default for Qt5" OFF )
option( SEPARATE_QJDNS "Build qjdns with iris library" OFF )
option( JINGLE_ICE "Enable ICE Jingle transport for p2p file transfer" ON )
option( BUNDLED_QCA "Adds: DTLS, Blake2b and other useful for XMPP crypto-stuff" OFF)
option( BUNDLED_USRSCTP "Compile compatible usrsctp lib when system one is not available or uncompatible (required for datachannel jingle transport)" OFF)
option( BUILD_IRIS_TOOLS "Build tools and examples" OFF)

set(CMAKE_CXX_STANDARD 14)

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

if(USE_QJDNS)
    message(WARNING "USE_QJDNS flag is enabled with Qt5. If you have problems with connection please disable this flag")
    add_definitions(-DNEED_JDNS)
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
    set(Qca_CORE_LIB ${QCA_BUILD_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}qca-qt5${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(Qca_OSSL_LIB ${QCA_BUILD_DIR}/lib/qca-qt5/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(Qca_LIBRARY ${Qca_OSSL_LIB} ${Qca_CORE_LIB})
    if(IS_SUBPROJECT)
        set(Qca_LIBRARY_EXPORT ${Qca_LIBRARY} PARENT_SCOPE)
        set(Qca_INCLUDE_DIR_EXPORT ${Qca_INCLUDE_DIR} PARENT_SCOPE)
    endif()

    find_package(OpenSSL REQUIRED)

    include(ExternalProject)
    #set CMake options and transfer the environment to an external project
    set(QCA_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF -DBUILD_PLUGINS=ossl -DLOAD_SHARED_PLUGINS=OFF
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${QCA_PREFIX} -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
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

find_package(UsrSCTP)
if (NOT UsrSCTP_FOUND AND (NOT BUNDLED_USRSCTP))
    message(FATAL_ERROR "UsrSCTP library not found. Try to install usrsctp library or enable BUNDLED_USRSCTP flag")
endif()

if (NOT UsrSCTP_FOUND AND BUNDLED_USRSCTP)
    message(STATUS "USRSCTP: using bundled")
    set(USRSCTP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/usrsctp)
    set(USRSCTP_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/usrsctp)
    set(USRSCTP_BUILD_DIR ${USRSCTP_PREFIX}/build)
    if(NOT EXISTS ${USRSCTP_SOURCE_DIR})
        list(APPEND USRSCTP_INCLUDE_DIR ${USRSCTP_PREFIX}/src/UsrSCTPProject/usrsctplib)
    else()
        list(APPEND USRSCTP_INCLUDE_DIR ${USRSCTP_SOURCE_DIR}/usrsctplib)
    endif()
    set(USRSCTP_LIBRARY ${USRSCTP_BUILD_DIR}/usrsctplib/${CMAKE_STATIC_LIBRARY_PREFIX}usrsctp${CMAKE_STATIC_LIBRARY_SUFFIX})
    find_package(Patch REQUIRED) #Temporary. Needed for patching usrsctp sources to disable shared libraries creation

    include(ExternalProject)
    #set CMake options and transfer the environment to an external project
    set(USRSCTP_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF -Dsctp_build_programs=OFF -Dsctp_debug=OFF -Dsctp_inet=OFF -Dsctp_inet6=OFF
        -DCMAKE_INSTALL_PREFIX=${USRSCTP_PREFIX} -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    if (EXISTS ${USRSCTP_SOURCE_DIR})
        message(STATUS "USRSCTP: found bundled sources")
        ExternalProject_Add(UsrSCTPProject
            PREFIX ${USRSCTP_PREFIX}
            BINARY_DIR ${USRSCTP_BUILD_DIR}
            SOURCE_DIR ${USRSCTP_SOURCE_DIR}
            CMAKE_ARGS ${USRSCTP_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${USRSCTP_LIBRARY}
            PATCH_COMMAND ${Patch_EXECUTABLE} ${USRSCTP_SOURCE_DIR}/usrsctplib/CMakeLists.txt ${CMAKE_CURRENT_SOURCE_DIR}/usrsctp_remove_shared.patch
            INSTALL_COMMAND ""
            )
    else()
        include(FindGit)
        find_package(Git)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "Git not found! Bundled UsrSCTP needs Git utility.\nPlease set GIT_EXECUTABLE variable or add git to PATH")
        endif()
        ExternalProject_Add(UsrSCTPProject
            PREFIX ${USRSCTP_PREFIX}
            BINARY_DIR ${USRSCTP_BUILD_DIR}
            GIT_REPOSITORY https://github.com/sctplab/usrsctp.git
            GIT_TAG f4925bd5ebb2db38263eef0ec816bf393293cf9c
            CMAKE_ARGS ${USRSCTP_BUILD_OPTIONS}
            PATCH_COMMAND ${Patch_EXECUTABLE} ${USRSCTP_PREFIX}/src/UsrSCTPProject/usrsctplib/CMakeLists.txt ${CMAKE_CURRENT_SOURCE_DIR}/usrsctp_remove_shared.patch
            INSTALL_COMMAND ""
            )
    endif()
    add_library(SctpLab::UsrSCTP UNKNOWN IMPORTED)
    set_target_properties(SctpLab::UsrSCTP PROPERTIES
            IMPORTED_LOCATION "${USRSCTP_LIBRARY}"
            INTERFACE_COMPILE_DEFINITIONS "${USRSCTP_DEFINITIONS}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    add_dependencies(SctpLab::UsrSCTP ${USRSCTP_LIBRARY})
endif()

set(sctpLab_LIBRARY ${USRSCTP_LIBRARY})
if(USRSCTP_INCLUDES)
    set(sctpLab_INCLUDES ${USRSCTP_INCLUDES})
else()
    set(sctpLab_INCLUDES ${USRSCTP_INCLUDE_DIR})
endif()
include_directories(
    ${sctpLab_INCLUDES}
)

if(NOT USE_QJDNS AND SEPARATE_QJDNS)
    message(FATAL_ERROR "SEPARATE_QJDNS flag enabled, but USE_QJDNS flag disabled.\nPlease enable flag USE_QJDNS or disable both flags")
endif()

if(USE_QJDNS AND (NOT SEPARATE_QJDNS))
    include(src/jdns.cmake)
elseif(USE_QJDNS)
    set(QJDns_SUFFIX -qt5)
    find_package(QJDns REQUIRED)
    set(QJDns_LIBRARY ${QJDns_LIBRARY} PARENT_SCOPE)
    include_directories( ${QJDns_INCLUDE_DIR} )
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
