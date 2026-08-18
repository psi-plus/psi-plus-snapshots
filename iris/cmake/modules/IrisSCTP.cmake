cmake_minimum_required(VERSION 3.10.0)

set(IRIS_USRSCTP_GIT_REPO "https://github.com/sctplab/usrsctp.git")
set(IRIS_USRSCTP_GIT_TAG 848eca82f92273af9a79687a90343a2ebcf3481d)

include(GNUInstallDirs)
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
    set(USRSCTP_PREFIX "${CMAKE_BINARY_DIR}/_deps/usrsctp")
    set(IRIS_USRSCTP_INSTALL_DIR "${USRSCTP_PREFIX}/install")
    set(USRSCTP_INCLUDE_DIR "${IRIS_USRSCTP_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}")
    set(USRSCTP_LIBRARY ${IRIS_USRSCTP_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}usrsctp${CMAKE_STATIC_LIBRARY_SUFFIX})
    if(WIN32 AND MSVC)
        add_definitions(-DWIN32_LEAN_AND_MEAN)
    endif()

    include(ExternalProject)
    #set CMake options and transfer the environment to an external project
    set(USRSCTP_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF -Dsctp_debug=OFF
        -Dsctp_inet=OFF -Dsctp_inet6=OFF -Dsctp_werror=OFF
        -DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR} "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        "-DLIB_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}"
        "-DINCLUDE_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_INCLUDEDIR}"
        "-DINSTALL_PKGCONFIG_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
    if (EXISTS ${USRSCTP_SOURCE_DIR})
        message(STATUS "USRSCTP: found bundled sources")
        set(_usrsctp_source_args SOURCE_DIR "${USRSCTP_SOURCE_DIR}" DOWNLOAD_COMMAND "")
    else()
        include(FindGit)
        find_package(Git)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "Git not found! Bundled UsrSCTP needs Git utility.\nPlease set GIT_EXECUTABLE variable or add git to PATH")
        endif()
        set(_usrsctp_source_args
            GIT_REPOSITORY ${IRIS_USRSCTP_GIT_REPO}
            GIT_TAG "${IRIS_USRSCTP_GIT_TAG}"
            GIT_PROGRESS TRUE
            )
        # When using the "cmake --build . -t clean" command, it cleans the built files, but the next time it builds, it crashes with a patch error.
        # As an attempt to avoid this crash the last line of patch_command was added
        include(patchSrcs)
        make_patch_command(patch_command
            SOURCE_DIR "<SOURCE_DIR>"
            PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules/usrsctp.patch"
            GIT_EXECUTABLE "${GIT_EXECUTABLE}"
            PATCH_APPLY_PATH "usrsctp.patch"
            CHECKOUT_FILES
                "usrsctplib/netinet/sctp_output.c"
        )
    endif()
    if(CMAKE_CONFIGURATION_TYPES)
        set(_usrsctp_build_config "$<CONFIG>")
    else()
        set(_usrsctp_build_config "${CMAKE_BUILD_TYPE}")
    endif()
    file(MAKE_DIRECTORY "${USRSCTP_INCLUDE_DIR}")
    ExternalProject_Add(UsrSCTPProject
        ${_usrsctp_source_args}
        UPDATE_COMMAND ""
        PREFIX ${USRSCTP_PREFIX}
        INSTALL_DIR ${IRIS_USRSCTP_INSTALL_DIR}
        CMAKE_ARGS ${USRSCTP_BUILD_OPTIONS}
        BUILD_BYPRODUCTS ${USRSCTP_LIBRARY}
        PATCH_COMMAND ${patch_command}
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config "${_usrsctp_build_config}"
        )
    add_library(SctpLab::UsrSCTP STATIC IMPORTED GLOBAL)
    set_target_properties(SctpLab::UsrSCTP PROPERTIES
            IMPORTED_LOCATION "${USRSCTP_LIBRARY}"
            INTERFACE_COMPILE_DEFINITIONS "${USRSCTP_DEFINITIONS}"
            INTERFACE_INCLUDE_DIRECTORIES "${USRSCTP_INCLUDE_DIR}"
            IMPORTED_LINK_INTERFACE_LANGUAGES "C")
    add_dependencies(SctpLab::UsrSCTP UsrSCTPProject)
endif()
