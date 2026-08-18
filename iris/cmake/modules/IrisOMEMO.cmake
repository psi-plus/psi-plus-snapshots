cmake_minimum_required(VERSION 3.10.0)

include(GNUInstallDirs)

set(IRIS_BUNDLED_OMEMO_C_VERSION "0.5.1" CACHE STRING "Bundled libomemo-c version")
set(IRIS_BUNDLED_PROTOBUF_C_VERSION "1.5.1" CACHE STRING "Bundled protobuf-c runtime version")
set(IRIS_OMEMO_C_SOURCE_DIR "" CACHE PATH "Local libomemo-c source directory")
set(IRIS_PROTOBUF_C_SOURCE_DIR "" CACHE PATH "Local protobuf-c source directory")

if(NOT IRIS_BUNDLED_OMEMO_C)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OmemoC REQUIRED IMPORTED_TARGET "libomemo-c>=0.5.1")
    return()
endif()

include(ExternalProject)
include(ProcessorCount)

processorcount(_iris_omemoc_detected_jobs)
if(NOT _iris_omemoc_detected_jobs)
    set(_iris_omemoc_detected_jobs 2)
endif()
set(IRIS_BUNDLED_OMEMO_C_JOBS "${_iris_omemoc_detected_jobs}" CACHE STRING
    "Parallel jobs used to build bundled libomemo-c dependencies")

set(_omemoc_prefix "${CMAKE_BINARY_DIR}/_deps/omemo-c")
set(IRIS_OMEMO_C_INSTALL_DIR "${_omemoc_prefix}/install")
set(_omemoc_library_dir "${IRIS_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}")
set(_omemoc_include_dir "${IRIS_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}/omemo")
set(_protobuf_c_include_root "${IRIS_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
set(_omemoc_library "${_omemoc_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}omemo-c${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_protobuf_c_library "${_omemoc_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}protobuf-c${CMAKE_STATIC_LIBRARY_SUFFIX}")

function(_iris_omemo_append_cross_compile_args output_var)
    set(_args ${${output_var}})
    foreach(_var IN ITEMS
            CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_STAGING_PREFIX CMAKE_FIND_ROOT_PATH
            CMAKE_OSX_SYSROOT CMAKE_OSX_ARCHITECTURES
            ANDROID_ABI ANDROID_PLATFORM ANDROID_NDK CMAKE_ANDROID_NDK
            CMAKE_ANDROID_ARCH_ABI CMAKE_ANDROID_API
            QT_HOST_PATH QT_HOST_PATH_CMAKE_DIR)
        if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
            string(REPLACE ";" "|" _value "${${_var}}")
            list(APPEND _args "-D${_var}=${_value}")
        endif()
    endforeach()
    set(${output_var} "${_args}" PARENT_SCOPE)
endfunction()

if(IRIS_PROTOBUF_C_SOURCE_DIR)
    if(NOT EXISTS "${IRIS_PROTOBUF_C_SOURCE_DIR}/protobuf-c/protobuf-c.c")
        message(FATAL_ERROR "IRIS_PROTOBUF_C_SOURCE_DIR does not contain a protobuf-c source tree: ${IRIS_PROTOBUF_C_SOURCE_DIR}")
    endif()
    set(_protobuf_c_source_args SOURCE_DIR "${IRIS_PROTOBUF_C_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
    set(_protobuf_c_source_args
        GIT_REPOSITORY https://github.com/protobuf-c/protobuf-c.git
        GIT_TAG "v${IRIS_BUNDLED_PROTOBUF_C_VERSION}"
        GIT_SHALLOW TRUE GIT_PROGRESS TRUE)
endif()

set(_protobuf_c_cmake_args
    "-DPROTOBUF_C_SOURCE_DIR=<SOURCE_DIR>" "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}" "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")
if(CMAKE_C_COMPILER)
    list(APPEND _protobuf_c_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
    list(APPEND _protobuf_c_cmake_args "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
_iris_omemo_append_cross_compile_args(_protobuf_c_cmake_args)

if(CMAKE_CONFIGURATION_TYPES)
    set(_omemoc_build_config "$<CONFIG>")
else()
    set(_omemoc_build_config "${CMAKE_BUILD_TYPE}")
endif()

ExternalProject_Add(
    iris_bundled_protobuf_c
    ${_protobuf_c_source_args}
    UPDATE_COMMAND ""
    PREFIX "${_omemoc_prefix}/protobuf-c"
    SOURCE_SUBDIR "."
    INSTALL_DIR "${IRIS_OMEMO_C_INSTALL_DIR}"
    LIST_SEPARATOR "|"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/protobuf-c-runtime" -B <BINARY_DIR>
                      ${_protobuf_c_cmake_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config "${_omemoc_build_config}" --parallel
                  "${IRIS_BUNDLED_OMEMO_C_JOBS}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config "${_omemoc_build_config}"
    BUILD_BYPRODUCTS "${_protobuf_c_library}")

if(IRIS_OMEMO_C_SOURCE_DIR)
    if(NOT EXISTS "${IRIS_OMEMO_C_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "IRIS_OMEMO_C_SOURCE_DIR does not contain a libomemo-c source tree: ${IRIS_OMEMO_C_SOURCE_DIR}")
    endif()
    set(_omemoc_source_args SOURCE_DIR "${IRIS_OMEMO_C_SOURCE_DIR}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
else()
    set(_omemoc_source_args
        GIT_REPOSITORY https://github.com/dino/libomemo-c.git
        GIT_TAG "v${IRIS_BUNDLED_OMEMO_C_VERSION}"
        GIT_SHALLOW TRUE GIT_PROGRESS TRUE
        UPDATE_COMMAND "")
endif()

set(_omemoc_c_flags "${CMAKE_C_FLAGS}")
string(APPEND _omemoc_c_flags " -I${IRIS_OMEMO_C_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
set(_omemoc_cmake_args
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DLIB_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}"
    "-DINCLUDE_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_INCLUDEDIR}"
    "-DINSTALL_PKGCONFIG_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DBUILD_SHARED_LIBS=OFF" "-DBUILD_TESTING=OFF" "-DCMAKE_C_FLAGS=${_omemoc_c_flags}")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    list(APPEND _omemoc_cmake_args "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
endif()
if(CMAKE_C_COMPILER)
    list(APPEND _omemoc_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_C_COMPILER_LAUNCHER)
    list(APPEND _omemoc_cmake_args "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
_iris_omemo_append_cross_compile_args(_omemoc_cmake_args)

if(WIN32 AND NOT MSVC)
    include(patchSrcs)
    make_patch_command(PATCH_CMD
        SOURCE_DIR "<SOURCE_DIR>"
        PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/mingw32-omemo-c.patch"
        GIT_EXECUTABLE "${GIT_EXECUTABLE}"
        PATCH_APPLY_PATH "mingw32-omemo-c.patch"
        CHECKOUT_FILES
            "src/signal_protocol.c"
            "src/signal_protocol_types.h"
    )
else()
    set(PATCH_CMD "")
endif()

ExternalProject_Add(
    iris_bundled_omemoc
    ${_omemoc_source_args}
    PREFIX "${_omemoc_prefix}/libomemo-c"
    INSTALL_DIR "${IRIS_OMEMO_C_INSTALL_DIR}"
    LIST_SEPARATOR "|"
    CMAKE_ARGS ${_omemoc_cmake_args}
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config "${_omemoc_build_config}" --parallel
                  "${IRIS_BUNDLED_OMEMO_C_JOBS}"
    INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config "${_omemoc_build_config}"
    BUILD_BYPRODUCTS "${_omemoc_library}"
    DEPENDS iris_bundled_protobuf_c
    PATCH_COMMAND ${PATCH_CMD}
    )

file(MAKE_DIRECTORY "${_omemoc_include_dir}" "${_protobuf_c_include_root}/protobuf-c")
add_library(IrisProtobufC STATIC IMPORTED GLOBAL)
set_target_properties(IrisProtobufC PROPERTIES
    IMPORTED_LOCATION "${_protobuf_c_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_protobuf_c_include_root}")
add_dependencies(IrisProtobufC iris_bundled_protobuf_c)

add_library(PkgConfig::OmemoC STATIC IMPORTED GLOBAL)
set_target_properties(PkgConfig::OmemoC PROPERTIES
    IMPORTED_LOCATION "${_omemoc_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_omemoc_include_dir}"
    INTERFACE_LINK_LIBRARIES IrisProtobufC)
add_dependencies(PkgConfig::OmemoC iris_bundled_omemoc)
