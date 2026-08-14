cmake_minimum_required(VERSION 3.16)
project(iris_protobuf_c_runtime LANGUAGES C)

include(GNUInstallDirs)

if(NOT PROTOBUF_C_SOURCE_DIR)
    message(FATAL_ERROR "PROTOBUF_C_SOURCE_DIR must point to a protobuf-c source tree")
endif()

set(_protobuf_c_source "${PROTOBUF_C_SOURCE_DIR}/protobuf-c/protobuf-c.c")
set(_protobuf_c_header "${PROTOBUF_C_SOURCE_DIR}/protobuf-c/protobuf-c.h")
if(NOT EXISTS "${_protobuf_c_source}" OR NOT EXISTS "${_protobuf_c_header}")
    message(FATAL_ERROR "PROTOBUF_C_SOURCE_DIR does not contain protobuf-c/protobuf-c.c and protobuf-c/protobuf-c.h: ${PROTOBUF_C_SOURCE_DIR}")
endif()

add_library(protobuf-c STATIC "${_protobuf_c_source}")
target_include_directories(protobuf-c PUBLIC "$<BUILD_INTERFACE:${PROTOBUF_C_SOURCE_DIR}>"
                                             "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(protobuf-c PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME protobuf-c)

install(TARGETS protobuf-c ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${_protobuf_c_header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/protobuf-c")

set(prefix "${CMAKE_INSTALL_PREFIX}")
set(exec_prefix "${CMAKE_INSTALL_PREFIX}")
set(libdir "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
set(includedir "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/libprotobuf-c.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/libprotobuf-c.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libprotobuf-c.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
