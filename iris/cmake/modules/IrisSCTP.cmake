cmake_minimum_required(VERSION 3.10.0)

set(IrisSCTPGitRepo "https://github.com/sctplab/usrsctp.git")

if(USE_MXE AND STDINT_FOUND)
    # Add SCTP_STDINT_INCLUDE definition to compile irisnet with usrsctp with MinGW
    add_definitions(
        -DSCTP_STDINT_INCLUDE="${STDINT_INCLUDE}"
    )
endif()

if(NOT IRIS_BUNDLED_USRSCTP)
    find_package(UsrSCTP)
    if(NOT UsrSCTP_FOUND)
        message(FATAL_ERROR "UsrSCTP library not found. Try to install usrsctp library or enable IRIS_BUNDLED_USRSCTP flag")
    endif()
else()
    message(STATUS "USRSCTP: using bundled")
    set(USRSCTP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/usrsctp)
    set(USRSCTP_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/usrsctp)
    set(USRSCTP_BUILD_DIR ${USRSCTP_PREFIX}/build)
    if(NOT EXISTS ${USRSCTP_SOURCE_DIR})
        set(USRSCTP_INCLUDES ${USRSCTP_PREFIX}/src/UsrSCTPProject/usrsctplib)
    else()
        set(USRSCTP_INCLUDES ${USRSCTP_SOURCE_DIR}/usrsctplib)
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
    # Setting these options seems to have no any effect because those prepended and not appended
    # if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    #     list(APPEND USRSCTP_BUILD_OPTIONS "-DCMAKE_C_FLAGS=-Wno-maybe-uninitialized")
    # endif()
    # if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    #     list(APPEND USRSCTP_BUILD_OPTIONS "-DCMAKE_C_FLAGS=-Wno-uninitialized -Wno-unused-but-set-variable")
    # endif()
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
        set(patch_command
        ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/usrsctp.patch <SOURCE_DIR> &&
        ${GIT_EXECUTABLE} checkout <SOURCE_DIR>/usrsctplib/netinet/sctp_output.c &&
        ${GIT_EXECUTABLE} apply <SOURCE_DIR>/usrsctp.patch)
        ExternalProject_Add(UsrSCTPProject
            PREFIX ${USRSCTP_PREFIX}
            BINARY_DIR ${USRSCTP_BUILD_DIR}
            GIT_REPOSITORY ${IrisSCTPGitRepo}
            GIT_TAG 848eca82f92273af9a79687a90343a2ebcf3481d
            CMAKE_ARGS ${USRSCTP_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${USRSCTP_LIBRARY}
            INSTALL_COMMAND ""
            PATCH_COMMAND ${patch_command}
            UPDATE_COMMAND ""
            )
    endif()
    add_library(SctpLab::UsrSCTP UNKNOWN IMPORTED)
    set_target_properties(SctpLab::UsrSCTP PROPERTIES
            IMPORTED_LOCATION "${USRSCTP_LIBRARY}"
            INTERFACE_COMPILE_DEFINITIONS "${USRSCTP_DEFINITIONS}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    add_dependencies(SctpLab::UsrSCTP ${USRSCTP_LIBRARY})
endif()
