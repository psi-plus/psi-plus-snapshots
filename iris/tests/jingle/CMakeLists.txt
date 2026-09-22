cmake_minimum_required(VERSION 3.16)

# This file is both a standalone CI project and the root Iris test subdirectory.
# Keep the historical standalone entry point, but do not recursively add Iris
# when the root project enabled IRIS_BUILD_TESTS.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(iris_jingle_regressions LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    option(USE_QT6 "Build with Qt 6" OFF)
    if(USE_QT6)
        set(QT_DEFAULT_MAJOR_VERSION 6)
    else()
        set(QT_DEFAULT_MAJOR_VERSION 5)
    endif()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(IRIS_ENABLE_OMEMO OFF CACHE BOOL "" FORCE)
    set(IRIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(../.. iris)
endif()

find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Network Xml)
add_executable(jingle_signaling_test signaling.cpp)
target_link_libraries(jingle_signaling_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
enable_testing()
add_executable(jingle_transports5b_test transports5b.cpp)
target_link_libraries(jingle_transports5b_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
# The white-box test compiles the real S5B implementation to exercise candidate
# state without an external proxy. Reuse its generated private QObject moc.
get_target_property(iris_binary_dir iris BINARY_DIR)
target_include_directories(jingle_transports5b_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${iris_binary_dir}/iris_autogen/include" "${iris_binary_dir}/iris_autogen/include_$<CONFIG>")
add_test(NAME jingle_transports5b COMMAND jingle_transports5b_test)
set_tests_properties(jingle_transports5b PROPERTIES TIMEOUT 15)
add_test(NAME jingle_signaling COMMAND jingle_signaling_test)
add_executable(jingle_tiebreaker_test tiebreaker.cpp)
target_link_libraries(jingle_tiebreaker_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_tiebreaker COMMAND jingle_tiebreaker_test)
add_executable(jingle_contentmodify_test contentmodify.cpp)
target_link_libraries(jingle_contentmodify_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_contentmodify COMMAND jingle_contentmodify_test)
add_executable(jingle_contentmodifyrace_test contentmodifyrace.cpp)
target_link_libraries(jingle_contentmodifyrace_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_contentmodifyrace COMMAND jingle_contentmodifyrace_test)
add_executable(jingle_contentmodifyiqboundary_test contentmodifyiqboundary.cpp)
target_link_libraries(jingle_contentmodifyiqboundary_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_contentmodify_iq_boundary COMMAND jingle_contentmodifyiqboundary_test)
add_executable(jingle_contentmodifycompletion_test contentmodifycompletion.cpp)
target_link_libraries(jingle_contentmodifycompletion_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_contentmodify_completion COMMAND jingle_contentmodifycompletion_test)
add_executable(jingle_directionpolicy_test directionpolicy.cpp)
target_link_libraries(jingle_directionpolicy_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_directionpolicy COMMAND jingle_directionpolicy_test)
add_executable(jingle_directionoperation_test directionoperation.cpp)
target_link_libraries(jingle_directionoperation_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_directionoperation COMMAND jingle_directionoperation_test)
set_tests_properties(jingle_directionoperation PROPERTIES TIMEOUT 10)
add_executable(jingle_sessioncallbacklifetime_test sessioncallbacklifetime.cpp)
target_link_libraries(jingle_sessioncallbacklifetime_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_sessioncallback_lifetime COMMAND jingle_sessioncallbacklifetime_test)
add_executable(jingle_contentmodifydispatcher_test contentmodifydispatcher.cpp)
target_link_libraries(jingle_contentmodifydispatcher_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_contentmodify_dispatcher COMMAND jingle_contentmodifydispatcher_test)
add_executable(jingle_dtlssrtp_test dtlssrtp.cpp)
# Dtls lives in irisnet but its fingerprint/XML helpers live in iris.
target_link_libraries(jingle_dtlssrtp_test PRIVATE irisnet iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_dtlssrtp COMMAND jingle_dtlssrtp_test)
add_executable(jingle_iceownership_test iceownership.cpp)
target_link_libraries(jingle_iceownership_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_iceownership COMMAND jingle_iceownership_test)
add_executable(jingle_icevalidpair_test icevalidpair.cpp)
target_link_libraries(jingle_icevalidpair_test PRIVATE irisnet iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network)
add_test(NAME jingle_icevalidpair COMMAND jingle_icevalidpair_test)
set_tests_properties(jingle_icevalidpair PROPERTIES TIMEOUT 12)
add_executable(jingle_iceudp_test iceudp.cpp)
target_include_directories(jingle_iceudp_test PRIVATE ../../src/xmpp/xmpp-im)
target_link_libraries(jingle_iceudp_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_iceudp COMMAND jingle_iceudp_test)
add_executable(jingle_domlifetime_test domlifetime.cpp)
target_include_directories(jingle_domlifetime_test PRIVATE ../../src/xmpp/xmpp-im)
target_link_libraries(jingle_domlifetime_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_domlifetime COMMAND jingle_domlifetime_test)
add_executable(jingle_messageinitiation_test messageinitiation.cpp)
target_link_libraries(jingle_messageinitiation_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_messageinitiation COMMAND jingle_messageinitiation_test)
add_executable(jingle_ftlifecycle_test ftlifecycle.cpp)
target_link_libraries(jingle_ftlifecycle_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_ftlifecycle COMMAND jingle_ftlifecycle_test)
add_executable(jingle_icereplacestate_test icereplacestate.cpp)
target_link_libraries(jingle_icereplacestate_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_icereplacestate COMMAND jingle_icereplacestate_test)
set_tests_properties(jingle_icereplacestate PROPERTIES TIMEOUT 15)
add_executable(jingle_transportselection_test transportselection.cpp)
target_link_libraries(jingle_transportselection_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_transportselection COMMAND jingle_transportselection_test)
add_executable(jingle_rtpcapsselection_test rtpcapsselection.cpp)
target_link_libraries(jingle_rtpcapsselection_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_rtpcapsselection COMMAND jingle_rtpcapsselection_test)
add_executable(jingle_transportreplace_test transportreplace.cpp)
target_link_libraries(jingle_transportreplace_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreplace COMMAND jingle_transportreplace_test)
add_executable(jingle_transportreplacematrix_test transportreplacematrix.cpp)
target_link_libraries(jingle_transportreplacematrix_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreplace_matrix COMMAND jingle_transportreplacematrix_test)
add_executable(jingle_transportacceptselfreentrancy_test transportacceptselfreentrancy.cpp)
target_link_libraries(jingle_transportacceptselfreentrancy_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportaccept_self_reentrancy COMMAND jingle_transportacceptselfreentrancy_test)
add_executable(jingle_transportrejectreentrancy_test transportrejectreentrancy.cpp)
target_link_libraries(jingle_transportrejectreentrancy_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreject_reentrancy COMMAND jingle_transportrejectreentrancy_test)
add_executable(jingle_transportrejectatomicity_test transportrejectatomicity.cpp)
target_link_libraries(jingle_transportrejectatomicity_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreject_atomicity COMMAND jingle_transportrejectatomicity_test)
add_executable(jingle_transportacceptatomicity_test transportacceptatomicity.cpp)
target_link_libraries(jingle_transportacceptatomicity_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportaccept_atomicity COMMAND jingle_transportacceptatomicity_test)
add_executable(jingle_transportacceptackreentrancy_test transportacceptackreentrancy.cpp)
target_link_libraries(jingle_transportacceptackreentrancy_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportaccept_ack_reentrancy COMMAND jingle_transportacceptackreentrancy_test)
add_executable(jingle_transportreplaceackboundary_test transportreplaceackboundary.cpp)
target_link_libraries(jingle_transportreplaceackboundary_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreplace_ack_boundary COMMAND jingle_transportreplaceackboundary_test)
add_executable(jingle_transportreplacedispatcher_test transportreplacedispatcher.cpp)
target_link_libraries(jingle_transportreplacedispatcher_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreplace_dispatcher COMMAND jingle_transportreplacedispatcher_test)
add_executable(jingle_transportreplacecorrectness_test transportreplacecorrectness.cpp)
target_link_libraries(jingle_transportreplacecorrectness_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreplace_inflight_without_unacked
    COMMAND jingle_transportreplacecorrectness_test inflight-without-unacked)
add_test(NAME jingle_transportreplace_unacked_without_replace
    COMMAND jingle_transportreplacecorrectness_test unacked-without-replace)
add_test(NAME jingle_transportreplace_failed_iq
    COMMAND jingle_transportreplacecorrectness_test failed-iq)
add_test(NAME jingle_transportreplace_malformed_accept
    COMMAND jingle_transportreplacecorrectness_test malformed-accept)
add_test(NAME jingle_transportreplace_duplicate_content
    COMMAND jingle_transportreplacecorrectness_test duplicate-content)
add_test(NAME jingle_transportreplace_missing_transport
    COMMAND jingle_transportreplacecorrectness_test missing-transport)
add_executable(jingle_transportreplaceprotocol_test transportreplaceprotocol.cpp)
target_link_libraries(jingle_transportreplaceprotocol_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_transportreplace_transport_reject
    COMMAND jingle_transportreplaceprotocol_test transport-reject)
add_test(NAME jingle_transportreplace_failed_iq_fallback
    COMMAND jingle_transportreplaceprotocol_test failed-iq-fallback)
add_test(NAME jingle_transportreplace_malformed_accept_session
    COMMAND jingle_transportreplaceprotocol_test malformed-accept)
add_test(NAME jingle_transportreplace_duplicate_accept
    COMMAND jingle_transportreplaceprotocol_test duplicate-accept)
add_test(NAME jingle_transportreplace_duplicate_reject
    COMMAND jingle_transportreplaceprotocol_test duplicate-reject)
add_executable(jingle_groupnegotiation_test groupnegotiation.cpp)
target_link_libraries(jingle_groupnegotiation_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_groupnegotiation COMMAND jingle_groupnegotiation_test)
add_executable(jingle_groupcommit_test groupcommit.cpp)
target_link_libraries(jingle_groupcommit_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_groupcommit COMMAND jingle_groupcommit_test)
add_executable(jingle_rtpdescription_test rtpdescription.cpp)
target_link_libraries(jingle_rtpdescription_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_rtpdescription COMMAND jingle_rtpdescription_test)
add_executable(jingle_rtpnegotiation_test rtpnegotiation.cpp)
target_link_libraries(jingle_rtpnegotiation_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_rtpnegotiation COMMAND jingle_rtpnegotiation_test)
add_executable(jingle_rtppreparedanswer_test rtppreparedanswer.cpp)
target_link_libraries(jingle_rtppreparedanswer_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_rtppreparedanswer COMMAND jingle_rtppreparedanswer_test)
add_executable(jingle_rtpmediaops_test rtpmediaops.cpp)
target_link_libraries(jingle_rtpmediaops_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_rtpmediaops COMMAND jingle_rtpmediaops_test)
add_executable(jingle_rtpapplication_test rtpapplication.cpp)
target_link_libraries(jingle_rtpapplication_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_rtpapplication COMMAND jingle_rtpapplication_test)
add_executable(jingle_rtpdirection_test rtpdirection.cpp)
target_link_libraries(jingle_rtpdirection_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_rtpdirection COMMAND jingle_rtpdirection_test)
add_executable(jingle_sessionacceptsubset_test sessionacceptsubset.cpp)
target_link_libraries(jingle_sessionacceptsubset_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME jingle_sessionacceptsubset COMMAND jingle_sessionacceptsubset_test)
add_executable(jingle_rtpruntimeerror_test rtpruntimeerror.cpp)
target_link_libraries(jingle_rtpruntimeerror_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_rtpruntimeerror COMMAND jingle_rtpruntimeerror_test)
if(IRIS_ENABLE_JINGLE_SCTP)
    target_compile_definitions(jingle_dtlssrtp_test PRIVATE IRIS_TEST_SCTP)
endif()
if(IRIS_ENABLE_JINGLE_SCTP)
    add_executable(jingle_transportsctp_test sctplifecycle.cpp)
    target_include_directories(jingle_transportsctp_test PRIVATE ../../src/xmpp/xmpp-im)
    target_link_libraries(jingle_transportsctp_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
        Qt${QT_DEFAULT_MAJOR_VERSION}::Network)
    add_test(NAME jingle_transportsctp_lifecycle COMMAND jingle_transportsctp_test)
    set_tests_properties(jingle_transportsctp_lifecycle PROPERTIES TIMEOUT 15)
endif()

add_executable(jingle_transportacks_test transportacks.cpp)
target_link_libraries(jingle_transportacks_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME jingle_transportacks COMMAND jingle_transportacks_test)
if(IRIS_SYSTEM_QCA STREQUAL "3")
    add_executable(jingle_icertp_test icertp.cpp)
    target_link_libraries(jingle_icertp_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
    add_test(NAME jingle_icertp COMMAND jingle_icertp_test)
    add_test(NAME jingle_icertp_iceudp COMMAND jingle_icertp_test --ice-udp)
    add_test(NAME jingle_rtpmedia COMMAND jingle_icertp_test --application)
    add_test(NAME jingle_rtpmedia_iceudp COMMAND jingle_icertp_test --application --ice-udp)
    add_executable(jingle_bundleice_test bundleice.cpp)
    target_link_libraries(jingle_bundleice_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
    add_test(NAME jingle_bundleice COMMAND jingle_bundleice_test)
    add_executable(jingle_bundlemedia_test bundlemedia.cpp)
    target_link_libraries(jingle_bundlemedia_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
    add_test(NAME jingle_bundlemedia COMMAND jingle_bundlemedia_test)
    add_executable(jingle_bundlesignaling_test bundlesignaling.cpp)
    target_link_libraries(jingle_bundlesignaling_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
    add_test(NAME jingle_bundlesignaling COMMAND jingle_bundlesignaling_test)
    set_tests_properties(jingle_icertp jingle_icertp_iceudp jingle_rtpmedia jingle_rtpmedia_iceudp PROPERTIES TIMEOUT 20)
    set_tests_properties(jingle_bundleice jingle_bundlemedia jingle_bundlesignaling PROPERTIES TIMEOUT 20)
endif()
