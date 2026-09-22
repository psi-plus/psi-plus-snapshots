cmake_minimum_required(VERSION 3.10.0)

find_package(Qt${QT_DEFAULT_MAJOR_VERSION} COMPONENTS Core Widgets REQUIRED)

if(Qt${QT_DEFAULT_MAJOR_VERSION}Core_FOUND)
    message(STATUS "Qt${QT_DEFAULT_MAJOR_VERSION} found, version ${Qt${QT_DEFAULT_MAJOR_VERSION}Core_VERSION}")
endif()

# Tell CMake to run moc when necessary:
set(CMAKE_AUTOMOC ON)
# As moc files are generated in the binary dir, tell CMake
# to always look for includes there:
set(CMAKE_INCLUDE_CURRENT_DIR ON)

get_filename_component(ABS_GST_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

option(PSIMEDIA_BUILD_TESTS "Build psimedia provider regression tests" OFF)
option(PSIMEDIA_ENABLE_SRTP "Enable direct libSRTP packet protection" ON)

set(PSIMEDIA_SRTP_TARGET "")
if(PSIMEDIA_ENABLE_SRTP)
    pkg_check_modules(PSIMEDIA_SRTP QUIET IMPORTED_TARGET libsrtp2>=2.0)
    if(TARGET PkgConfig::PSIMEDIA_SRTP)
        set(PSIMEDIA_SRTP_TARGET PkgConfig::PSIMEDIA_SRTP)
    else()
        find_path(PSIMEDIA_SRTP_INCLUDE_DIR
                  NAMES srtp2/srtp.h
                  HINTS ${SRTP_ROOT} $ENV{SRTP_ROOT} ${GST_SDK}
                  PATH_SUFFIXES include)
        find_library(PSIMEDIA_SRTP_LIBRARY
                     NAMES srtp2
                     HINTS ${SRTP_ROOT} $ENV{SRTP_ROOT} ${GST_SDK}
                     PATH_SUFFIXES lib lib64)
        if(NOT PSIMEDIA_SRTP_INCLUDE_DIR OR NOT PSIMEDIA_SRTP_LIBRARY)
            message(FATAL_ERROR
                "PSIMEDIA_ENABLE_SRTP=ON requires libSRTP 2.x (libsrtp2). "
                "Install the development package, set SRTP_ROOT, or disable SRTP explicitly.")
        endif()
        add_library(psimedia_srtp2 UNKNOWN IMPORTED)
        set_target_properties(psimedia_srtp2 PROPERTIES
                              IMPORTED_LOCATION "${PSIMEDIA_SRTP_LIBRARY}"
                              INTERFACE_INCLUDE_DIRECTORIES "${PSIMEDIA_SRTP_INCLUDE_DIR}")
        set(PSIMEDIA_SRTP_TARGET psimedia_srtp2)
    endif()
endif()

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR ("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo"))
    option(PSIMEDIA_PIPELINE_DEBUG "Add -DPIPELINE_DEBUG definition" OFF)
    option(PSIMEDIA_RTPWORKER_DEBUG "Add -DRTPWORKER_DEBUG definition" OFF)
    if(PSIMEDIA_PIPELINE_DEBUG)
        add_definitions(-DPIPELINE_DEBUG)
    endif()
    if(PSIMEDIA_RTPWORKER_DEBUG)
        add_definitions(-DRTPWORKER_DEBUG)
    endif()
endif()

#search Glib modules
pkg_check_modules(GLIBMODULES REQUIRED
                    glib-2.0
                    gobject-2.0
                    gthread-2.0
)
#search Gstreamer modules
pkg_check_modules(GSTMODULES REQUIRED
                    gstreamer-1.0
                    gstreamer-app-1.0
                    gstreamer-base-1.0
                    gstreamer-audio-1.0
                    gstreamer-video-1.0
)

set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/devices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/modes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/payloadinfo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pipeline.cpp
    ${CMAKE_CURRENT_LIST_DIR}/bins.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtpsessionbridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtpsessionbridge.h
    ${CMAKE_CURRENT_LIST_DIR}/rtpbundlerouter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtpbundlerouter.h
    ${CMAKE_CURRENT_LIST_DIR}/rtpgroupbridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtpgroupbridge.h
    ${CMAKE_CURRENT_LIST_DIR}/securertpcontext.cpp
    ${CMAKE_CURRENT_LIST_DIR}/securertpcontext.h
    ${CMAKE_CURRENT_LIST_DIR}/securertpgroup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/securertpgroup.h
    ${CMAKE_CURRENT_LIST_DIR}/rtpworker.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rtpworker_devices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstthread.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rwcontrol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstprovider.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstprovider.h
    ${CMAKE_CURRENT_LIST_DIR}/gstrtpsessioncontext.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstrtpsessioncontext.h
    ${CMAKE_CURRENT_LIST_DIR}/gstvideowidget.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstvideowidget.h
    ${CMAKE_CURRENT_LIST_DIR}/gstrtpchannel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstrtpchannel.h
    ${CMAKE_CURRENT_LIST_DIR}/gstrecorder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstrecorder.h
    ${CMAKE_CURRENT_LIST_DIR}/gstfeaturescontext.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstfeaturescontext.h
    ${CMAKE_CURRENT_LIST_DIR}/gstaudiorecordercontext.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstaudiorecordercontext.h
)

if(UNIX AND NOT APPLE)
    list(APPEND SOURCES ${CMAKE_CURRENT_LIST_DIR}/devices/deviceenum_unix.cpp)
endif()

if(NOT BUILD_PSIPLUGIN)
    set(PSIMEDIA_INCDIR ${ABS_GST_PARENT_DIR}/psimedia)
    include_directories(${PSIMEDIA_INCDIR})
else()
    find_package(PsiPluginsApi REQUIRED)
    include_directories(
        ${PsiPluginsApi_INCLUDE_DIR}
    )
endif()


add_definitions(
    ${GLIBMODULES_CFLAGS}
    ${GLIBMODULES_CFLAGS_OTHER}
    ${GSTMODULES_CFLAGS}
    ${GSTMODULES_CFLAGS_OTHER}
)

link_directories(
    ${GSTMODULES_LIBRARY_DIRS}
    ${GLIBMODULES_LIBRARY_DIRS}
)

set(GST_INCLUDES
    ${GSTMODULES_INCLUDE_DIRS}
    ${GLIBMODULES_INCLUDE_DIRS}
)

set(CMAKE_MODULE_LINKER_FLAGS
   "${CMAKE_MODULE_LINKER_FLAGS} \
    ${GLIBMODULES_LDFLAGS_OTHER} \
    ${GSTMODULES_LDFLAGS_OTHER}"
)

if(WIN32)
    list(APPEND PROJECT_LDFLAGS
        gstaudio-1.0
        gstrtp-1.0
        opus
        setupapi
        ksuser
        dsound
        ole32
    )
    if(USE_MXE OR MSVC)
        list(APPEND PROJECT_LDFLAGS
            amstrmid
        )
    endif()
    if(NOT MSVC)
        list(APPEND PROJECT_LDFLAGS
            dxerr9
        )
    endif()
endif()

set(PROVIDERLIB gstprovidersrc)
add_library(${PROVIDERLIB} STATIC ${SOURCES})

if(${QT_DEFAULT_MAJOR_VERSION} GREATER_EQUAL 6)
    set_target_properties(${PROVIDERLIB} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(${PROVIDERLIB}
                           PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/devices
                           ${PSIMEDIA_INCDIR}
                           ${GST_INCLUDES}
)
target_link_libraries(${PROVIDERLIB} PUBLIC
                      ${GSTMODULES_LINK_LIBRARIES}
                      ${GLIBMODULES_LINK_LIBRARIES}
                      Qt${QT_DEFAULT_MAJOR_VERSION}::Core
                      Qt${QT_DEFAULT_MAJOR_VERSION}::Gui
)

target_link_libraries(${PROVIDERLIB} PRIVATE
    ${PROJECT_LDFLAGS}
    Qt${QT_DEFAULT_MAJOR_VERSION}::Widgets
)

if(PSIMEDIA_ENABLE_SRTP)
    target_compile_definitions(${PROVIDERLIB} PRIVATE PSIMEDIA_HAVE_SRTP=1)
    # gstprovidersrc is static, so the SRTP dependency must propagate to every
    # plugin/test executable that consumes it.
    target_link_libraries(${PROVIDERLIB} PUBLIC ${PSIMEDIA_SRTP_TARGET})
endif()

if(APPLE)
    target_link_libraries(${PROVIDERLIB} PRIVATE "-framework CoreAudio")
    set_property(TARGET ${PROVIDERLIB}  PROPERTY SUFFIX ".dylib")
endif()

if(PSIMEDIA_BUILD_TESTS)
    enable_testing()

    add_executable(psimedia_rtpgroupbridge_test ${ABS_GST_PARENT_DIR}/tests/rtpgroupbridge.cpp)
    target_link_libraries(psimedia_rtpgroupbridge_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpgroupbridge COMMAND psimedia_rtpgroupbridge_test)
    set_tests_properties(psimedia_rtpgroupbridge PROPERTIES TIMEOUT 20)

    add_executable(psimedia_rtpbundlerouter_test ${ABS_GST_PARENT_DIR}/tests/rtpbundlerouter.cpp)
    target_link_libraries(psimedia_rtpbundlerouter_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpbundlerouter COMMAND psimedia_rtpbundlerouter_test)
    set_tests_properties(psimedia_rtpbundlerouter PROPERTIES TIMEOUT 10)

    if(PSIMEDIA_ENABLE_SRTP)
        add_executable(psimedia_securertpqueue_test ${ABS_GST_PARENT_DIR}/tests/securertpqueue.cpp)
        target_link_libraries(psimedia_securertpqueue_test PRIVATE ${PROVIDERLIB})
        add_test(NAME psimedia_securertpqueue COMMAND psimedia_securertpqueue_test)
        set_tests_properties(psimedia_securertpqueue PROPERTIES TIMEOUT 10)

        add_executable(psimedia_securertpgroup_test ${ABS_GST_PARENT_DIR}/tests/securertpgroup.cpp)
        target_link_libraries(psimedia_securertpgroup_test PRIVATE ${PROVIDERLIB})
        add_test(NAME psimedia_securertpgroup COMMAND psimedia_securertpgroup_test)
        set_tests_properties(psimedia_securertpgroup PROPERTIES TIMEOUT 25)

        add_executable(psimedia_securertp_test ${ABS_GST_PARENT_DIR}/tests/securertp.cpp)
        target_link_libraries(psimedia_securertp_test PRIVATE ${PROVIDERLIB})
        add_test(NAME psimedia_securertp COMMAND psimedia_securertp_test)
        set_tests_properties(psimedia_securertp PROPERTIES TIMEOUT 15)
    endif()

    add_executable(psimedia_rwcontrol_teardown_test ${ABS_GST_PARENT_DIR}/tests/rwcontrol_teardown.cpp)
    target_link_libraries(psimedia_rwcontrol_teardown_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rwcontrol_teardown COMMAND psimedia_rwcontrol_teardown_test)
    set_tests_properties(psimedia_rwcontrol_teardown PROPERTIES TIMEOUT 10)

    add_executable(psimedia_rtpworkerownership_test ${ABS_GST_PARENT_DIR}/tests/rtpworkerownership.cpp)
    target_link_libraries(psimedia_rtpworkerownership_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpworkerownership COMMAND psimedia_rtpworkerownership_test)
    set_tests_properties(psimedia_rtpworkerownership PROPERTIES TIMEOUT 20)

    add_executable(psimedia_rtpworker_receive_order_test ${ABS_GST_PARENT_DIR}/tests/rtpworker_receive_order.cpp)
    target_link_libraries(psimedia_rtpworker_receive_order_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpworker_receive_order COMMAND psimedia_rtpworker_receive_order_test)
    set_tests_properties(psimedia_rtpworker_receive_order PROPERTIES TIMEOUT 20)

    add_executable(psimedia_rtpworker_send_order_test ${ABS_GST_PARENT_DIR}/tests/rtpworker_send_order.cpp)
    target_link_libraries(psimedia_rtpworker_send_order_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpworker_send_order COMMAND psimedia_rtpworker_send_order_test)
    set_tests_properties(psimedia_rtpworker_send_order PROPERTIES TIMEOUT 45)

    add_executable(psimedia_opusrtppay_test ${ABS_GST_PARENT_DIR}/tests/opusrtppay.cpp)
    target_link_libraries(psimedia_opusrtppay_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_opusrtppay COMMAND psimedia_opusrtppay_test)
    set_tests_properties(psimedia_opusrtppay PROPERTIES TIMEOUT 15)

    add_executable(psimedia_payloadinfo_test ${ABS_GST_PARENT_DIR}/tests/payloadinfo.cpp)
    target_link_libraries(psimedia_payloadinfo_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_payloadinfo COMMAND psimedia_payloadinfo_test)
    set_tests_properties(psimedia_payloadinfo PROPERTIES TIMEOUT 10)

    add_executable(psimedia_rtpchannel_test ${ABS_GST_PARENT_DIR}/tests/rtpchannel.cpp)
    target_link_libraries(psimedia_rtpchannel_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpchannel COMMAND psimedia_rtpchannel_test)
    set_tests_properties(psimedia_rtpchannel PROPERTIES TIMEOUT 10)

    add_executable(psimedia_gstvideowidget_size_test ${ABS_GST_PARENT_DIR}/tests/gstvideowidget_size.cpp)
    target_link_libraries(psimedia_gstvideowidget_size_test PRIVATE
                          ${PROVIDERLIB}
                          Qt${QT_DEFAULT_MAJOR_VERSION}::Widgets)
    add_test(NAME psimedia_gstvideowidget_size COMMAND psimedia_gstvideowidget_size_test)
    set_tests_properties(psimedia_gstvideowidget_size PROPERTIES
                         TIMEOUT 10
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(psimedia_videoprep_test ${ABS_GST_PARENT_DIR}/tests/videoprep.cpp)
    target_link_libraries(psimedia_videoprep_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_videoprep COMMAND psimedia_videoprep_test)
    set_tests_properties(psimedia_videoprep PROPERTIES TIMEOUT 10)

    add_executable(psimedia_video_capture_caps_test ${ABS_GST_PARENT_DIR}/tests/video_capture_caps.cpp)
    target_link_libraries(psimedia_video_capture_caps_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_video_capture_caps COMMAND psimedia_video_capture_caps_test)
    set_tests_properties(psimedia_video_capture_caps PROPERTIES TIMEOUT 10)

    add_executable(psimedia_rtpsessioncontext_sender_test ${ABS_GST_PARENT_DIR}/tests/rtpsessioncontext_sender.cpp)
    target_link_libraries(psimedia_rtpsessioncontext_sender_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpsessioncontext_sender COMMAND psimedia_rtpsessioncontext_sender_test)
    set_tests_properties(psimedia_rtpsessioncontext_sender PROPERTIES TIMEOUT 45)

    add_executable(psimedia_rtpsessionbridge_test ${ABS_GST_PARENT_DIR}/tests/rtpsessionbridge.cpp)
    target_link_libraries(psimedia_rtpsessionbridge_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpsessionbridge COMMAND psimedia_rtpsessionbridge_test)
    set_tests_properties(psimedia_rtpsessionbridge PROPERTIES TIMEOUT 15)

    add_executable(psimedia_rtpsessionbridge_packets_test ${ABS_GST_PARENT_DIR}/tests/rtpsessionbridge_packets.cpp)
    target_link_libraries(psimedia_rtpsessionbridge_packets_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpsessionbridge_packets COMMAND psimedia_rtpsessionbridge_packets_test)
    set_tests_properties(psimedia_rtpsessionbridge_packets PROPERTIES TIMEOUT 15)

    add_executable(psimedia_rtpsessionbridge_periodic_test ${ABS_GST_PARENT_DIR}/tests/rtpsessionbridge_periodic.cpp)
    target_link_libraries(psimedia_rtpsessionbridge_periodic_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpsessionbridge_periodic COMMAND psimedia_rtpsessionbridge_periodic_test)
    set_tests_properties(psimedia_rtpsessionbridge_periodic PROPERTIES TIMEOUT 15)

    add_executable(psimedia_rtpsessionbridge_backpressure_test ${ABS_GST_PARENT_DIR}/tests/rtpsessionbridge_backpressure.cpp)
    target_link_libraries(psimedia_rtpsessionbridge_backpressure_test PRIVATE ${PROVIDERLIB})
    add_test(NAME psimedia_rtpsessionbridge_backpressure COMMAND psimedia_rtpsessionbridge_backpressure_test)
    set_tests_properties(psimedia_rtpsessionbridge_backpressure PROPERTIES TIMEOUT 15)
endif()
