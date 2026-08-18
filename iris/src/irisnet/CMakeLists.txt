cmake_minimum_required(VERSION 3.10.0)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_SOURCE_DIR}/cmake/modules"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
)

add_library(irisnet STATIC)

set(IRISNET_CORELIB_HEADERS
    corelib/addressresolver.h
    corelib/irisnetexport.h
    corelib/irisnetglobal.h
    corelib/irisnetplugin.h
    corelib/netavailability.h
    corelib/netinterface.h
    corelib/netnames.h
    corelib/objectsession.h
)
set(IRISNET_NONCORE_HEADERS
    noncore/dtls.h
    noncore/ice176.h
    noncore/iceabstractstundisco.h
    noncore/iceagent.h
    noncore/icecomponent.h
    noncore/icetransport.h
    noncore/processquit.h
    noncore/stunallocate.h
    noncore/stunbinding.h
    noncore/stunmessage.h
    noncore/stuntransaction.h
    noncore/tcpportreserver.h
    noncore/transportaddress.h
    noncore/turnclient.h
    noncore/udpportreserver.h
)
set(IRISNET_CUTESTUFF_HEADERS
    noncore/cutestuff/bsocket.h
    noncore/cutestuff/bytestream.h
    noncore/cutestuff/httpconnect.h
    noncore/cutestuff/httppoll.h
    noncore/cutestuff/socks.h
)
set(IRISNET_LEGACY_HEADERS
    noncore/legacy/ndns.h
    noncore/legacy/srvresolver.h
)

target_sources(irisnet PRIVATE
    ${IRISNET_CORELIB_HEADERS}
    ${IRISNET_NONCORE_HEADERS}
    ${IRISNET_CUTESTUFF_HEADERS}
    ${IRISNET_LEGACY_HEADERS}
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
    corelib/netinterface_qtname.cpp
    corelib/netinterface_qtnet.cpp

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
    noncore/dtls.cpp

    noncore/cutestuff/bsocket.cpp
)

if(UNIX)
    target_sources(irisnet PRIVATE
        corelib/netinterface_unix.cpp
    )
endif()

if(IRIS_ENABLE_JINGLE_SCTP)
    target_sources(irisnet PRIVATE
        noncore/sctp/SctpAssociation.cpp
        noncore/sctp/DepUsrSCTP.cpp
    )
endif()

set(Qca_INCLUDE_DIR "")

if(IRIS_BUNDLED_QCA)
    add_dependencies(irisnet Qca::Qca)
    get_target_property(Qca_INCLUDE_DIR Qca::Qca INTERFACE_INCLUDE_DIRECTORIES)
endif()
list(APPEND IRISNET_DEPS "$<BUILD_INTERFACE:Qca::Qca>")

if(IRIS_ENABLE_JINGLE_SCTP)
    if(IRIS_BUNDLED_USRSCTP)
        add_dependencies(irisnet SctpLab::UsrSCTP)
        get_target_property(USRSCTP_INCLUDES SctpLab::UsrSCTP INTERFACE_INCLUDE_DIRECTORIES)
    endif()
    list(APPEND IRISNET_DEPS "$<BUILD_INTERFACE:SctpLab::UsrSCTP>")
endif()

target_include_directories(irisnet
    PRIVATE
        ${iris_SOURCE_DIR}/include/iris
        ${iris_SOURCE_DIR}/src
        ${Qca_INCLUDE_DIR}
    PUBLIC
        ${USRSCTP_INCLUDES}
)

target_compile_definitions(irisnet PRIVATE IRISNET_STATIC HAVE_QTNET)

if(WIN32)
    set(EXTRA_LDFLAGS ws2_32 iphlpapi)
endif()

# UsrSCTP requires pthread
if(NOT WIN32)
    find_package(Threads)
    target_link_libraries(irisnet PUBLIC Threads::Threads)
endif()

target_link_libraries(irisnet PUBLIC
    ${IRISNET_DEPS}
)

if(QT_DEFAULT_MAJOR_VERSION LESS 6)
    target_link_libraries(irisnet PUBLIC Qt5::Core Qt5::Network Qt5::Xml)
else()
    target_link_libraries(irisnet PUBLIC Qt6::Core Qt6::Network Qt6::Xml)
endif()
target_link_libraries(irisnet PUBLIC ${EXTRA_LDFLAGS})

if(IRIS_ENABLE_INSTALL)
    install(FILES ${IRISNET_CORELIB_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/irisnet/corelib
    )
    install(FILES ${IRISNET_NONCORE_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/irisnet/noncore
    )
    install(FILES ${IRISNET_CUTESTUFF_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/irisnet/noncore/cutestuff
    )
    install(FILES ${IRISNET_LEGACY_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/irisnet/noncore/legacy
    )
endif()
