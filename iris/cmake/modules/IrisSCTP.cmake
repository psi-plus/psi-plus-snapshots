cmake_minimum_required(VERSION 3.2.0)

include(CMakeDependentOption)
cmake_dependent_option( BUNDLED_USRSCTP
    "Compile compatible usrsctp lib when system one is not available or uncompatible (required for datachannel jingle transport)"
    OFF "JINGLE_SCTP" OFF)

if((CMAKE_CROSSCOMPILING AND DEFINED MSYS) AND STDINT_FOUND)
    #Add SCTP_STDINT_INCLUDE definition to compile irisnet with usrsctp with MinGW
    add_definitions(
        -DSCTP_STDINT_INCLUDE="${STDINT_INCLUDE}"
    )
endif()

if(NOT BUNDLED_USRSCTP)
    find_package(UsrSCTP)
    if (NOT UsrSCTP_FOUND)
        message(FATAL_ERROR "UsrSCTP library not found. Try to install usrsctp library or enable BUNDLED_USRSCTP flag")
    endif()
else()
    message(STATUS "USRSCTP: using bundled")
    set(USRSCTP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/usrsctp)
    set(USRSCTP_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/usrsctp)
    set(USRSCTP_BUILD_DIR ${USRSCTP_PREFIX}/build)
    if(NOT EXISTS ${USRSCTP_SOURCE_DIR})
        list(APPEND USRSCTP_INCLUDE_DIR ${USRSCTP_PREFIX}/src/UsrSCTPProject/usrsctplib)
    else()
        list(APPEND USRSCTP_INCLUDE_DIR ${USRSCTP_SOURCE_DIR}/usrsctplib)
    endif()
    set(USRSCTP_LIBRARY ${USRSCTP_BUILD_DIR}/usrsctplib/${CMAKE_STATIC_LIBRARY_PREFIX}usrsctp${CMAKE_STATIC_LIBRARY_SUFFIX})
    if(WIN32 AND MSVC)
        add_definitions(-DWIN32_LEAN_AND_MEAN)
    endif()

    include(ExternalProject)
    #set CMake options and transfer the environment to an external project
    set(USRSCTP_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF -Dsctp_debug=OFF
        -Dsctp_inet=OFF -Dsctp_inet6=OFF -DCMAKE_INSTALL_PREFIX=${USRSCTP_PREFIX}
        -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND USRSCTP_BUILD_OPTIONS -DCMAKE_C_FLAGS="-Wno-maybe-uninitialized")
    endif()
    if (EXISTS ${USRSCTP_SOURCE_DIR})
        message(STATUS "USRSCTP: found bundled sources")
        ExternalProject_Add(UsrSCTPProject
            PREFIX ${USRSCTP_PREFIX}
            BINARY_DIR ${USRSCTP_BUILD_DIR}
            SOURCE_DIR ${USRSCTP_SOURCE_DIR}
            CMAKE_ARGS ${USRSCTP_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${USRSCTP_LIBRARY}
            INSTALL_COMMAND ""
            )
    else()
        include(FindGit)
        find_package(Git)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "Git not found! Bundled UsrSCTP needs Git utility.\nPlease set GIT_EXECUTABLE variable or add git to PATH")
        endif()
        ExternalProject_Add(UsrSCTPProject
            PREFIX ${USRSCTP_PREFIX}
            BINARY_DIR ${USRSCTP_BUILD_DIR}
            GIT_REPOSITORY https://github.com/sctplab/usrsctp.git
            GIT_TAG a17109528c75d01f6372d5c30851a639684c6e99
            CMAKE_ARGS ${USRSCTP_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${USRSCTP_LIBRARY}
            INSTALL_COMMAND ""
            )
    endif()
    add_library(SctpLab::UsrSCTP UNKNOWN IMPORTED)
    set_target_properties(SctpLab::UsrSCTP PROPERTIES
            IMPORTED_LOCATION "${USRSCTP_LIBRARY}"
            INTERFACE_COMPILE_DEFINITIONS "${USRSCTP_DEFINITIONS}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    add_dependencies(SctpLab::UsrSCTP ${USRSCTP_LIBRARY})
endif()

set(sctpLab_LIBRARY ${USRSCTP_LIBRARY})
if(USRSCTP_INCLUDES)
    set(sctpLab_INCLUDES ${USRSCTP_INCLUDES})
else()
    set(sctpLab_INCLUDES ${USRSCTP_INCLUDE_DIR})
endif()
include_directories(
    ${sctpLab_INCLUDES}
)
add_definitions(-DJINGLE_SCTP)
