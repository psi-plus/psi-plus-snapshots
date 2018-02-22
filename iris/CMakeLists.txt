if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

project(iris)
cmake_minimum_required(VERSION 3.1.0)

set( IRIS_LIB_VERSION_MAJOR 1 )
set( IRIS_LIB_VERSION_MINOR 0 )
set( IRIS_LIB_VERSION_PATCH 0 )
set( IRIS_LIB_VERSION_STRING
    ${IRIS_LIB_VERSION_MAJOR}.${IRIS_LIB_VERSION_MINOR}.${IRIS_LIB_VERSION_PATCH}
    )

set( CMAKE_MODULE_PATH
    "${CMAKE_MODULE_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/modules"
    "${PROJECT_SOURCE_DIR}/cmake/modules"
    "${PROJECT_SOURCE_DIR}/../cmake/modules"
    )

option( USE_QJDNS "Use qjdns/jdns library. Disabled by default for Qt5" OFF )
option( SEPARATE_QJDNS "Build qjdns with iris library" OFF )

include_directories(${PROJECT_SOURCE_DIR} ${PROJECT_BINARY_DIR})
include_directories(include/iris)

set(CMAKE_AUTOMOC OFF)

find_package(Qt5 COMPONENTS Core Gui Xml Network REQUIRED)
add_definitions(-DHAVE_QT5)
if(USE_QJDNS)
    message(WARNING "USE_QJDNS flag is enabled with Qt5. If you have problems with connection please disable this flag")
endif()

if(USE_QJDNS)
    add_definitions(-DNEED_JDNS)
endif()

find_package(Qca REQUIRED)
set(QCA_INCLUDES ${Qca_INCLUDE_DIR})
set(qca_LIB ${Qca_LIBRARY})

include_directories(
    ${QCA_INCLUDES}
)

macro(qt_wrap_cpp)
    set(_SOURCES ${ARGN})
    list(REMOVE_AT _SOURCES 0)
    foreach(SOURCE ${_SOURCES})
        if(SOURCE MATCHES "^(.*/|)([^/.]+)\\.h$")
            set(DEST "moc_${CMAKE_MATCH_2}.cpp")
            qt5_generate_moc(${SOURCE} ${DEST})
            list(APPEND ${ARGV0} ${DEST})
        elseif(SOURCE MATCHES "^(.*/|)([^/.]+)\\.cpp$")
            set(DEST "${CMAKE_MATCH_2}.moc")
            qt5_generate_moc(${SOURCE} ${DEST})
            list(APPEND ${ARGV0} ${DEST})
        else()
            message(WARNING "Wrong source ${SOURCE}")
        endif()
    endforeach()
    unset(_SOURCES)
endmacro()

if(NOT USE_QJDNS AND SEPARATE_QJDNS)
    message(WARNING "SEPARATE_QJDNS flag enabled, but USE_QJDNS flag disabled. Both flags will be disabled")
    set(SEPARATE_QJDNS OFF)
endif()

if(USE_QJDNS AND (NOT SEPARATE_QJDNS))
    add_definitions(-DJDNS_STATIC)
    set(QJDns_LIBRARY qjdns)
    include_directories(
        src/jdns/include/jdns
        )
    set(jdns_SRCS
        src/jdns/src/jdns/jdns.c
        src/jdns/src/jdns/jdns_mdnsd.c
        src/jdns/src/jdns/jdns_packet.c
        src/jdns/src/jdns/jdns_sys.c
        src/jdns/src/jdns/jdns_util.c
        )
    set(jdns_PUBLIC_HEADERS
        src/jdns/include/jdns/jdns.h
        src/jdns/include/jdns/jdns_export.h
        )
    set(jdns_HEADERS
        src/jdns/src/jdns/jdns_packet.h
        src/jdns/src/jdns/jdns_mdnsd.h
        src/jdns/src/jdns/jdns_p.h
        )
    add_library(jdns STATIC ${jdns_SRCS} ${jdns_HEADERS} ${jdns_PUBLIC_HEADERS})
    if(WIN32)
        target_link_libraries(jdns ws2_32 advapi32)
    endif()
    set(qjdns_MOC_HDRS
        src/jdns/include/jdns/qjdns.h
        src/jdns/include/jdns/qjdnsshared.h
        src/jdns/src/qjdns/qjdns_p.h
        src/jdns/src/qjdns/qjdnsshared_p.h
        )

    qt_wrap_cpp(qjdns_MOC_SRCS ${qjdns_MOC_HDRS})

    set(qjdns_SRCS
        src/jdns/src/qjdns/qjdns.cpp
        src/jdns/src/qjdns/qjdns_sock.cpp
        src/jdns/src/qjdns/qjdnsshared.cpp
        )

    set(qjdns_PUBLIC_HEADERS
        src/jdns/include/jdns/qjdns.h
        src/jdns/include/jdns/qjdnsshared.h
        )
    set(qjdns_HEADERS
        src/jdns/src/qjdns/qjdns_sock.h
        )
    add_library(${QJDns_LIBRARY} STATIC ${qjdns_SRCS} ${qjdns_MOC_SRCS} ${qjdns_MOC_HDRS} ${qjdns_PUBLIC_HEADERS})
    target_link_libraries(${QJDns_LIBRARY} Qt5::Core Qt5::Network)
    target_link_libraries(${QJDns_LIBRARY} jdns)
elseif(USE_QJDNS)
    set(QJDns_SUFFIX -qt5)
    find_package(QJDns REQUIRED)
    set(QJDns_LIBRARY ${QJDns_LIBRARY} PARENT_SCOPE)
    include_directories( ${QJDns_INCLUDE_DIR} )
endif()

find_package(IDN REQUIRED)

add_definitions(-DIRISNET_STATIC)

add_subdirectory(src/irisnet)
add_subdirectory(src/xmpp)
