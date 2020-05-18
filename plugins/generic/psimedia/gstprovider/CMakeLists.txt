cmake_minimum_required(VERSION 3.1.0)

find_package(Qt5 COMPONENTS Core Widgets REQUIRED)

if(Qt5Core_FOUND)
    message(STATUS "Qt5 found, version ${Qt5Core_VERSION}")
endif()

# Tell CMake to run moc when necessary:
set(CMAKE_AUTOMOC ON)
# As moc files are generated in the binary dir, tell CMake
# to always look for includes there:
set(CMAKE_INCLUDE_CURRENT_DIR ON)

get_filename_component(ABS_GST_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

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
    ${CMAKE_CURRENT_LIST_DIR}/rtpworker.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstthread.cpp
    ${CMAKE_CURRENT_LIST_DIR}/rwcontrol.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gstprovider.cpp
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

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/devices
)

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
target_include_directories(${PROVIDERLIB}
                           PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/devices
                           ${PSIMEDIA_INCDIR}
                           ${GST_INCLUDES}
)
target_link_libraries(${PROVIDERLIB}
                      ${GSTMODULES_LINK_LIBRARIES}
                      ${GLIBMODULES_LINK_LIBRARIES}
                      ${PROJECT_LDFLAGS}
)

if(APPLE)
    target_link_libraries(${PROVIDERLIB} "-framework CoreAudio")
    set_property(TARGET ${PROVIDERLIB}  PROPERTY SUFFIX ".dylib")
endif()

target_link_libraries(${PROVIDERLIB} Qt5::Core Qt5::Gui Qt5::Widgets)
