cmake_minimum_required(VERSION 3.2.0)
if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
    message(STATUS "CMP0074 policy set to NEW")
endif()

add_definitions(-DXMPP_TEST)

find_package(ZLIB REQUIRED)
get_filename_component(ABS_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(ABS_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ${ABS_PARENT_DIR}
    ${ABS_PARENT_DIR}/irisnet/corelib
    xmpp-core
    xmpp-im
    ${ZLIB_INCLUDE_DIRS}
    ${QCA_INCLUDES}
    )

set(HEADERS
    xmpp-core/parser.h
    xmpp-core/protocol.h
    xmpp-core/sm.h
    xmpp-core/td.h
    xmpp-core/xmlprotocol.h
    xmpp-core/xmpp_stanza.h

    xmpp-im/xmpp_address.h
    xmpp-im/xmpp_hash.h
    xmpp-im/xmpp_thumbs.h
    xmpp-im/xmpp_agentitem.h
    xmpp-im/xmpp_captcha.h
    xmpp-im/xmpp_chatstate.h
    xmpp-im/xmpp_discoitem.h
    xmpp-im/xmpp_features.h
    xmpp-im/xmpp_form.h
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
    xmpp-im/xmpp_xdata.h
    xmpp-im/xmpp_xmlcommon.h

    base/randomnumbergenerator.h
    base/randrandomnumbergenerator.h
    base/timezone.h

    xmpp-im/im.h

    jid/jid.h

    sasl/digestmd5proplist.h
    sasl/digestmd5response.h
    sasl/plainmessage.h
    sasl/scramsha1message.h
    sasl/scramsha1response.h
    sasl/scramsha1signature.h

    blake2/blake2qt.h

    xmpp-core/compressionhandler.h
    xmpp-core/securestream.h
    xmpp-core/xmpp.h
    xmpp-core/xmpp_clientstream.h
    xmpp-core/xmpp_stream.h

    xmpp-im/xmpp_caps.h
    xmpp-im/filetransfer.h
    xmpp-im/httpfileupload.h
    xmpp-im/s5b.h
    xmpp-im/xmpp_bitsofbinary.h
    xmpp-im/xmpp_bytestream.h
    xmpp-im/xmpp_client.h
    xmpp-im/xmpp_discoinfotask.h
    xmpp-im/xmpp_ibb.h
    xmpp-im/xmpp_serverinfomanager.h
    xmpp-im/xmpp_task.h
    xmpp-im/xmpp_tasks.h
    xmpp-im/jingle.h
    xmpp-im/jingle-transport.h
    xmpp-im/jingle-nstransportslist.h
    xmpp-im/jingle-application.h
    xmpp-im/jingle-session.h
    xmpp-im/jingle-ft.h
    xmpp-im/jingle-ice.h
    xmpp-im/jingle-s5b.h
    xmpp-im/jingle-ibb.h

    zlib/zlibcompressor.h
    zlib/zlibdecompressor.h
    )

set(SOURCES
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
    xmpp-im/xmpp_discoinfotask.cpp
    xmpp-im/xmpp_discoitem.cpp
    xmpp-im/xmpp_ibb.cpp
    xmpp-im/xmpp_reference.cpp
    xmpp-im/xmpp_serverinfomanager.cpp
    xmpp-im/xmpp_subsets.cpp
    xmpp-im/xmpp_task.cpp
    xmpp-im/xmpp_tasks.cpp
    xmpp-im/xmpp_vcard.cpp
    xmpp-im/xmpp_xdata.cpp
    xmpp-im/xmpp_xmlcommon.cpp
    xmpp-im/jingle.cpp
    xmpp-im/jingle-session.cpp
    xmpp-im/jingle-application.cpp
    xmpp-im/jingle-transport.cpp
    xmpp-im/jingle-nstransportslist.cpp
    xmpp-im/jingle-ft.cpp
    xmpp-im/jingle-ice.cpp
    xmpp-im/jingle-s5b.cpp
    xmpp-im/jingle-ibb.cpp

    base/randomnumbergenerator.cpp
    base/timezone.cpp

    zlib/zlibcompressor.cpp
    zlib/zlibdecompressor.cpp

    blake2/blake2qt.cpp

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

if(NOT MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11")
endif()

add_library(iris
    STATIC
    ${HEADERS}
    ${SOURCES}
)

set_property(TARGET iris PROPERTY C_STANDARD 11)

if(B2_FOUND)
    message(STATUS "Building with system blake2 library")
    target_compile_definitions(iris PUBLIC IRIS_SYSTEM_BLAKE2)
    target_link_libraries(iris ${B2_LIBRARY})
else()
    if (NOT BUNDLED_QCA)
        message(STATUS "No system blake2 and bundled QCA is disabled. Expect slow hashing.")
    endif()
    target_sources(iris PRIVATE
            blake2/blake2b-ref.c
            blake2/blake2s-ref.c
            blake2/blake2.h
            blake2/blake2-impl.h)
    target_include_directories(iris PRIVATE blake2)
endif()


if( SEPARATE_QJDNS AND WIN32)
    set(EXTRA_LDFLAGS ws2_32)
endif()

target_link_libraries(iris ${ZLIB_LIBRARY} ${EXTRA_LDFLAGS} stringprep)
target_link_libraries(iris irisnet Qt5::Core Qt5::Gui Qt5::Network Qt5::Xml)
if (BUNDLED_QCA)
    add_dependencies(iris QcaProject)
    target_link_libraries(iris Qca)
    target_include_directories(iris INTERFACE ${Qca_INCLUDE_DIR})
else()
    target_link_libraries(iris ${qca_LIB})
endif()

target_compile_definitions(iris INTERFACE IRISNET_STATIC)
target_include_directories(iris INTERFACE ${ABS_ROOT_DIR}/include ${ABS_ROOT_DIR}/include/iris ${ABS_PARENT_DIR})
