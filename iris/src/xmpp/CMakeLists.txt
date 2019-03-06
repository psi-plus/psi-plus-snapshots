add_definitions(-DXMPP_TEST)

find_package(ZLIB REQUIRED)
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_LIST_DIR}
    ..
    ../irisnet/corelib
    xmpp-core
    xmpp-im
    ${IDN_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${QCA_INCLUDES}
    )

set(PLAIN_HEADERS
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
    xmpp-im/xmpp_htmlelement.h
    xmpp-im/xmpp_httpauthrequest.h
    xmpp-im/xmpp_liveroster.h
    xmpp-im/xmpp_liverosteritem.h
    xmpp-im/xmpp_message.h
    xmpp-im/xmpp_muc.h
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
    blake2/blake2.h
    blake2/blake2-impl.h
    )

set(HEADERS
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
    xmpp-im/jingle-ft.h
    xmpp-im/jingle-s5b.h

    zlib/zlibcompressor.h
    zlib/zlibdecompressor.h
    )

set(PLAIN_SOURCES
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
    xmpp-im/xmpp_serverinfomanager.cpp
    xmpp-im/xmpp_subsets.cpp
    xmpp-im/xmpp_task.cpp
    xmpp-im/xmpp_tasks.cpp
    xmpp-im/xmpp_vcard.cpp
    xmpp-im/xmpp_xdata.cpp
    xmpp-im/xmpp_xmlcommon.cpp
    xmpp-im/jingle-s5b.cpp
    xmpp-im/jingle-ft.cpp

    base/randomnumbergenerator.cpp
    base/timezone.cpp

    zlib/zlibcompressor.cpp
    zlib/zlibdecompressor.cpp

    blake2/blake2qt.cpp
    blake2/blake2b-ref.c
    blake2/blake2s-ref.c

    jid/jid.cpp

    sasl/digestmd5proplist.cpp
    sasl/digestmd5response.cpp
    sasl/plainmessage.cpp
    sasl/scramsha1message.cpp
    sasl/scramsha1response.cpp
    sasl/scramsha1signature.cpp
    )

set(SOURCES
    xmpp-core/securestream.cpp
    xmpp-core/simplesasl.cpp

    xmpp-im/s5b.cpp
    xmpp-im/xmpp_features.cpp
    xmpp-im/jingle.cpp
    )

qt_wrap_cpp(MOC_SOURCES ${HEADERS} ${SOURCES})
add_library(iris
    STATIC
    ${HEADERS}
    ${SOURCES}
    ${MOC_SOURCES}
    ${PLAIN_SOURCES}
    ${PLAIN_HEADERS}
)

if( SEPARATE_QJDNS AND WIN32)
    set(EXTRA_LDFLAGS ws2_32)
endif()

target_link_libraries(iris ${IDN_LIBRARY} ${ZLIB_LIBRARY} ${EXTRA_LDFLAGS})
target_link_libraries(iris irisnet Qt5::Core Qt5::Gui Qt5::Network Qt5::Xml ${qca_LIB})
target_compile_definitions(iris INTERFACE IRISNET_STATIC)
target_include_directories(iris INTERFACE ../../include ../../include/iris ..)
