cmake_minimum_required(VERSION 3.10.0)

project(gstprovider LANGUAGES CXX)

get_directory_property(IS_SUBPROJECT PARENT_DIRECTORY)

set(CMAKE_CXX_STANDARD 17)

set( CMAKE_MODULE_PATH
    ${CMAKE_MODULE_PATH}
    ${PROJECT_SOURCE_DIR}/cmake/modules
)
#Detect MXE cross-compilation
if(EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    string(TOLOWER ${CMAKE_TOOLCHAIN_FILE} TOOLCHAIN_FILE)
    string(REGEX MATCH "mxe-conf" MXE_DETECTED "${TOOLCHAIN_FILE}")
    if(MXE_DETECTED)
        message(STATUS "MXE environment detected")
        option(USE_MXE "Use MXE toolkit" ON)
        message(STATUS "MXE root path: ${CMAKE_PREFIX_PATH}")
    endif()
endif()

if(POLICY CMP0053)
    cmake_policy(SET CMP0053 NEW)
endif()

# On Windows debug library should have 'd' postfix.
if(WIN32)
    unset(GST_SDK)
    set(CMAKE_DEBUG_POSTFIX "d")
    if(NOT USE_MXE)
        if(CMAKE_SIZEOF_VOID_P MATCHES "8")
            if(MSVC)
                set(GST_SDK_PATH $ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64} CACHE STRING "Path to gstreamer SDK")
            else()
                set(GST_SDK_PATH $ENV{GSTREAMER_1_0_ROOT_X86_64}  CACHE STRING "Path to gstreamer SDK")
            endif()
        else()
            if(MSVC)
                set(GST_SDK_PATH $ENV{GSTREAMER_1_0_ROOT_MSVC_X86} CACHE STRING "Path to gstreamer SDK")
            else()
                set(GST_SDK_PATH $ENV{GSTREAMER_1_0_ROOT_x86}  CACHE STRING "Path to gstreamer SDK")
            endif()
        endif()
        string(REPLACE "\\" "/" GST_SDK ${GST_SDK_PATH})
        message(STATUS "GST_SDK: ${GST_SDK}")
        if(GST_SDK)
            set(ENV{PKG_CONFIG_PATH} "${GST_SDK}/lib/pkgconfig")
            set(GST_BIN_DIR "${GST_SDK}/bin")
            set(GST_LIB_DIR "${GST_SDK}/lib")
            set(GST_INC_DIR "${GST_SDK}/include")
            include_directories(
                ${GST_LIB_DIR}
                ${GST_INC_DIR}
            )
            link_directories(
                ${GST_BIN_DIR}
                ${GST_LIB_DIR}
            )
            #try to find pkg-config executable in GST_BIN_DIR
            find_program(PKG_EXEC pkg-config PATHS ${GST_BIN_DIR})
            message(STATUS "PKG_EXEC: ${PKG_EXEC}")
            if(NOT "${PKG_EXEC}" STREQUAL "PKG_EXEC-NOTFOUND")
                set(PKG_CONFIG_EXECUTABLE ${PKG_EXEC} CACHE STRING "Path to pkg-config")
            endif()
        endif()
    endif()
elseif(APPLE)
    set(CMAKE_DEBUG_POSTFIX "_debug")
endif()

include(FindPkgConfig)
find_package(PkgConfig REQUIRED)

option(USE_PSI "Use gstprovider module for Psi client. Should be disabled for Psi+ client" ON)
option(BUILD_DEMO "Build psimedia-demo" ON)
option(BUILD_PSIPLUGIN "Build a regular Psi plugin" ON)

if(NOT DEFINED USE_PSI)
    if(MAIN_PROGRAM_NAME AND (${MAIN_PROGRAM_NAME} STREQUAL "psi"))
        set(USE_PSI ON)
    else()
        set(USE_PSI OFF)
    endif()
elseif(USE_PSI)
    set(MAIN_PROGRAM_NAME "psi")
elseif(NOT USE_PSI)
    set(MAIN_PROGRAM_NAME "psi-plus")
endif()
set(CLIENT_NAME ${MAIN_PROGRAM_NAME})

message(STATUS "${PROJECT_NAME} module will be installed in plugins directory for client: \"${CLIENT_NAME}\"")

#add extra search paths for libraries and includes
if(WIN32)
    set(BIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}" CACHE STRING "Directory where binary will be installed")
    set(LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/plugins" CACHE STRING "Directory where plugin will be installed")
else(WIN32)
    set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
    set(BIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/bin" CACHE STRING "Directory where binary will be installed")
    set(LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}/${CLIENT_NAME}/plugins" CACHE STRING "Directory where plugin will be installed")
endif(WIN32)

if(NOT IS_SUBPROJECT)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/psimedia")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/")
    find_program(CLF_BIN clang-format DOC "Path to clang-format binary")
    if(CLF_BIN)
    #Obtain list of source files
        file(GLOB_RECURSE SRC_LIST
            *.c
            *.cc
            *.cpp
            *.hpp
            *.h
            *.mm
        )
        add_custom_target(fix-codestyle
            COMMAND ${CLF_BIN}
            --verbose
            -style=file
            -i ${SRC_LIST}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Fix codestyle with clang-format"
            VERBATIM
        )
    endif()
endif()

if("${QT_DEFAULT_MAJOR_VERSION}" STREQUAL "")
    set(QT_DEFAULT_MAJOR_VERSION 5)
endif()

if(BUILD_DEMO)
    add_subdirectory(demo)
    add_subdirectory(gstplugin)
endif()
if(BUILD_PSIPLUGIN)
    add_subdirectory(psiplugin)
endif()
add_subdirectory(gstprovider)
