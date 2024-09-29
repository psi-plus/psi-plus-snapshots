cmake_minimum_required(VERSION 3.10.0)

find_package(ZLIB REQUIRED)

if(IS_SUBPROJECT)
    add_library(iris STATIC)
else()
    add_library(iris)
endif()

set(XMPP_JID_HEADERS jid/jid.h)

set(XMPP_CORE_HEADERS
    xmpp-core/parser.h
    xmpp-core/protocol.h
    xmpp-core/sm.h
    xmpp-core/td.h
    xmpp-core/xmlprotocol.h
    xmpp-core/xmpp_stanza.h

    xmpp-core/compressionhandler.h
    xmpp-core/securestream.h
    xmpp-core/xmpp.h
    xmpp-core/xmpp_clientstream.h
    xmpp-core/xmpp_stream.h
)

set(XMPP_IM_HEADERS
    xmpp-im/xmpp_address.h
    xmpp-im/xmpp_hash.h
    xmpp-im/xmpp_thumbs.h
    xmpp-im/xmpp_agentitem.h
    xmpp-im/xmpp_captcha.h
    xmpp-im/xmpp_carbons.h
    xmpp-im/xmpp_chatstate.h
    xmpp-im/xmpp_discoitem.h
    xmpp-im/xmpp_features.h
    xmpp-im/xmpp_form.h
    xmpp-im/xmpp_forwarding.h
    xmpp-im/xmpp_htmlelement.h
    xmpp-im/xmpp_httpauthrequest.h
    xmpp-im/xmpp_liveroster.h
    xmpp-im/xmpp_liverosteritem.h
    xmpp-im/xmpp_message.h
    xmpp-im/xmpp_muc.h
    xmpp-im/xmpp_reference.h
    xmpp-im/xmpp_pubsubitem.h
    xmpp-im/xmpp_pubsubretraction.h
    xmpp-im/xmpp_receipts.h
    xmpp-im/xmpp_resource.h
    xmpp-im/xmpp_resourcelist.h
    xmpp-im/xmpp_roster.h
    xmpp-im/xmpp_rosteritem.h
    xmpp-im/xmpp_rosterx.h
    xmpp-im/xmpp_status.h
    xmpp-im/xmpp_subsets.h
    xmpp-im/xmpp_url.h
    xmpp-im/xmpp_vcard.h
    xmpp-im/xmpp_vcard4.h
    xmpp-im/xmpp_xdata.h
    xmpp-im/xmpp_xmlcommon.h
    xmpp-im/xmpp_encryption.h
    xmpp-im/xmpp_externalservicediscovery.h
    xmpp-im/stundisco.h
    xmpp-im/im.h
    xmpp-im/xmpp_caps.h
    xmpp-im/filetransfer.h
    xmpp-im/httpfileupload.h
    xmpp-im/s5b.h
    xmpp-im/xmpp_bitsofbinary.h
    xmpp-im/xmpp_bytestream.h
    xmpp-im/xmpp_client.h
    xmpp-im/xmpp_discoinfotask.h
    xmpp-im/xmpp_mamtask.h
    xmpp-im/xmpp_mammanager.h
    xmpp-im/xmpp_ibb.h
    xmpp-im/xmpp_serverinfomanager.h
    xmpp-im/xmpp_task.h
    xmpp-im/xmpp_tasks.h
    xmpp-im/jingle.h
    xmpp-im/jingle-connection.h
    xmpp-im/jingle-transport.h
    xmpp-im/jingle-nstransportslist.h
    xmpp-im/jingle-application.h
    xmpp-im/jingle-session.h
    xmpp-im/jingle-ft.h
    xmpp-im/jingle-ice.h
    xmpp-im/jingle-s5b.h
    xmpp-im/jingle-ibb.h
    xmpp-im/jingle-file.h
    xmpp-im/jingle-sctp.h
)

set(XMPP_HEADERS_PRIVATE
    sasl/digestmd5proplist.h
    sasl/digestmd5response.h
    sasl/plainmessage.h
    sasl/scramsha1message.h
    sasl/scramsha1response.h
    sasl/scramsha1signature.h
    zlib/zlibcompressor.h
    zlib/zlibdecompressor.h
    base/timezone.h
)

target_sources(iris PRIVATE
    ${XMPP_JID_HEADERS}
    ${XMPP_CORE_HEADERS}
    ${XMPP_IM_HEADERS}
    ${XMPP_HEADERS_PRIVATE}
    xmpp-core/compressionhandler.cpp
    xmpp-core/connector.cpp
    xmpp-core/parser.cpp
    xmpp-core/protocol.cpp
    xmpp-core/sm.cpp
    xmpp-core/stream.cpp
    xmpp-core/tlshandler.cpp
    xmpp-core/xmlprotocol.cpp
    xmpp-core/xmpp_stanza.cpp

    xmpp-im/client.cpp
    xmpp-im/filetransfer.cpp
    xmpp-im/httpfileupload.cpp
    xmpp-im/types.cpp
    xmpp-im/xmpp_bitsofbinary.cpp
    xmpp-im/xmpp_bytestream.cpp
    xmpp-im/xmpp_caps.cpp
    xmpp-im/xmpp_carbons.cpp
    xmpp-im/xmpp_discoinfotask.cpp
    xmpp-im/xmpp_discoitem.cpp
    xmpp-im/xmpp_hash.cpp
    xmpp-im/xmpp_ibb.cpp
    xmpp-im/xmpp_forwarding.cpp
    xmpp-im/xmpp_mamtask.cpp
    xmpp-im/xmpp_mammanager.cpp
    xmpp-im/xmpp_reference.cpp
    xmpp-im/xmpp_serverinfomanager.cpp
    xmpp-im/xmpp_subsets.cpp
    xmpp-im/xmpp_task.cpp
    xmpp-im/xmpp_tasks.cpp
    xmpp-im/xmpp_vcard.cpp
    xmpp-im/xmpp_vcard4.cpp
    xmpp-im/xmpp_xdata.cpp
    xmpp-im/xmpp_xmlcommon.cpp
    xmpp-im/xmpp_encryption.cpp
    xmpp-im/xmpp_externalservicediscovery.cpp
    xmpp-im/stundisco.cpp

    xmpp-im/jingle.cpp
    xmpp-im/jingle-connection.cpp
    xmpp-im/jingle-session.cpp
    xmpp-im/jingle-application.cpp
    xmpp-im/jingle-transport.cpp
    xmpp-im/jingle-nstransportslist.cpp
    xmpp-im/jingle-ft.cpp
    xmpp-im/jingle-ice.cpp
    xmpp-im/jingle-s5b.cpp
    xmpp-im/jingle-ibb.cpp
    xmpp-im/jingle-file.cpp

    base/timezone.cpp

    zlib/zlibcompressor.cpp
    zlib/zlibdecompressor.cpp

    jid/jid.cpp

    sasl/digestmd5proplist.cpp
    sasl/digestmd5response.cpp
    sasl/plainmessage.cpp
    sasl/scramsha1message.cpp
    sasl/scramsha1response.cpp
    sasl/scramsha1signature.cpp

    xmpp-core/securestream.cpp
    xmpp-core/simplesasl.cpp

    xmpp-im/s5b.cpp
    xmpp-im/xmpp_features.cpp
)

if(IRIS_ENABLE_JINGLE_SCTP)
    target_compile_definitions(iris PRIVATE JINGLE_SCTP)
    target_sources(iris PRIVATE
        xmpp-im/jingle-sctp.cpp
        xmpp-im/jingle-sctp-association_p.cpp
        xmpp-im/jingle-sctp-association_p.h
        xmpp-im/jingle-webrtc-datachannel_p.cpp
        xmpp-im/jingle-webrtc-datachannel_p.h
    )
endif()

if(QT_DEFAULT_MAJOR_VERSION LESS 6)
    target_sources(iris PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/blake2/blake2qt.cpp ${CMAKE_CURRENT_SOURCE_DIR}/blake2/blake2qt.h)
    if(B2_FOUND)
        message(STATUS "Building with system blake2 library")
        target_link_libraries(iris PRIVATE ${B2_LIBRARY})
    else()
        if(NOT IRIS_BUNDLED_QCA)
            message(STATUS "No system blake2 and bundled QCA is disabled. Expect slow hashing.")
        endif()
        target_sources(iris PRIVATE
            blake2/blake2b-ref.c
            blake2/blake2s-ref.c
        )
    endif()
endif()

target_link_libraries(iris
    PRIVATE
        $<BUILD_INTERFACE:stringprep>
        $<BUILD_INTERFACE:irisnet>
        ZLIB::ZLIB
    PUBLIC
        ${Qca_LIBRARY}
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Xml
)

if(IRIS_BUNDLED_QCA)
    add_dependencies(iris QcaProject)
    target_include_directories(iris PUBLIC
        "$<BUILD_INTERFACE:${Qca_INCLUDE_DIR}>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/iris/qca>"
    )
    target_link_libraries(iris PUBLIC OpenSSL::SSL)
endif()

if(IRIS_ENABLE_DEBUG)
    target_compile_definitions(iris PRIVATE XMPP_TEST)
endif()

target_compile_definitions(iris PUBLIC IRISNET_STATIC QSTRINGPREP_BUILDING)

if(IS_SUBPROJECT)
    target_include_directories(iris
        PUBLIC
            ${iris_SOURCE_DIR}/include
            ${iris_SOURCE_DIR}/src
            ${Qca_INCLUDE_DIR}
        )
    target_include_directories(iris
        PRIVATE
            ${iris_SOURCE_DIR}/include/iris
        )
else()
    target_include_directories(iris
        PRIVATE
            ${iris_SOURCE_DIR}/include
            ${iris_SOURCE_DIR}/include/iris
            ${iris_SOURCE_DIR}/src
            ${Qca_INCLUDE_DIR}
        )
endif()
target_include_directories(iris PUBLIC $<INSTALL_INTERFACE:include/xmpp>)

if (IRIS_ENABLE_INSTALL)
    install(FILES ${XMPP_JID_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/xmpp/jid
    )
    install(FILES ${XMPP_CORE_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/xmpp/xmpp-core
    )
    install(FILES ${XMPP_IM_HEADERS}
        DESTINATION ${IRIS_INSTALL_INCLUDEDIR}/xmpp/xmpp-im
    )
endif()
