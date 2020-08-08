cmake_minimum_required(VERSION 3.1.0)
get_filename_component(ABS_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    corelib
    noncore
    noncore/cutestuff
    noncore/legacy
    ${QCA_INCLUDES}
)

set( CMAKE_MODULE_PATH
    "${CMAKE_MODULE_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
    "${ABS_PARENT_DIR}/cmake/modules"
)

set(SOURCES
    corelib/irisnetglobal.cpp
    corelib/irisnetplugin.cpp

    noncore/icetransport.cpp
    noncore/stunmessage.cpp
    noncore/stuntypes.cpp
    noncore/stunutil.cpp

    noncore/cutestuff/bytestream.cpp
    noncore/cutestuff/httpconnect.cpp
    noncore/cutestuff/httppoll.cpp
    noncore/cutestuff/socks.cpp

    noncore/legacy/ndns.cpp
    noncore/legacy/srvresolver.cpp

    corelib/addressresolver.cpp
    corelib/netavailability.cpp
    corelib/netinterface.cpp
    corelib/netnames.cpp
    corelib/objectsession.cpp

    noncore/iceagent.cpp
    noncore/ice176.cpp
    noncore/icecomponent.cpp
    noncore/icelocaltransport.cpp
    noncore/iceturntransport.cpp
    noncore/processquit.cpp
    noncore/stunallocate.cpp
    noncore/stunbinding.cpp
    noncore/stuntransaction.cpp
    noncore/turnclient.cpp
    noncore/udpportreserver.cpp
    noncore/tcpportreserver.cpp

    noncore/cutestuff/bsocket.cpp
)

if(UNIX)
    list(APPEND SOURCES corelib/netinterface_unix.cpp)
endif()

if(NOT USE_QJDNS)
    list(APPEND SOURCES corelib/netinterface_qtname.cpp)
else()
    list(APPEND SOURCES corelib/netnames_jdns.cpp)
endif()
if (Qt5Network_VERSION VERSION_LESS 5.15.0)
    list(APPEND SOURCES corelib/netinterface_qtnet.cpp)
endif()

set(HEADERS
    corelib/irisnetexport.h
    corelib/irisnetglobal.h
    corelib/irisnetglobal_p.h

    noncore/stunmessage.h
    noncore/stuntypes.h
    noncore/stunutil.h

    corelib/addressresolver.h
    corelib/irisnetplugin.h
    corelib/netavailability.h
    corelib/netinterface.h
    corelib/netnames.h
    corelib/objectsession.h

    noncore/iceagent.h
    noncore/ice176.h
    noncore/icecomponent.h
    noncore/icelocaltransport.h
    noncore/icetransport.h
    noncore/iceturntransport.h
    noncore/processquit.h
    noncore/stunallocate.h
    noncore/stunbinding.h
    noncore/stuntransaction.h
    noncore/turnclient.h
    noncore/udpportreserver.h
    noncore/tcpportreserver.h

    noncore/cutestuff/bsocket.h
    noncore/cutestuff/bytestream.h
    noncore/cutestuff/httpconnect.h
    noncore/cutestuff/httppoll.h
    noncore/cutestuff/socks.h

    noncore/legacy/ndns.h
    noncore/legacy/srvresolver.h
)

add_library(irisnet STATIC ${SOURCES} ${HEADERS} )

if(WIN32 AND (SEPARATE_QJDNS OR (NOT USE_QJDNS)))
    set(EXTRA_LDFLAGS ws2_32)
endif()

if(NOT USE_QJDNS)
    set(QJDns_LIBRARY "")
endif()

if (Qt5Network_VERSION VERSION_LESS 5.15.0)
    target_compile_definitions(irisnet PRIVATE HAVE_QTNET)
endif()

target_link_libraries(irisnet ${QJDns_LIBRARY} ${EXTRA_LDFLAGS})
target_link_libraries(irisnet Qt5::Core Qt5::Network Qt5::Xml ${qca_LIB})
if (BUNDLED_QCA)
    add_dependencies(irisnet qca)
endif()
