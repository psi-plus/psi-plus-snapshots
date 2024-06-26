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
    ${CMAKE_CURRENT_LIST_DIR}/rtpworker.cpp
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
)

target_link_libraries(${PROVIDERLIB} PRIVATE ${PROJECT_LDFLAGS})

if(APPLE)
    target_link_libraries(${PROVIDERLIB} PRIVATE "-framework CoreAudio")
    set_property(TARGET ${PROVIDERLIB}  PROPERTY SUFFIX ".dylib")
endif()

target_link_libraries(${PROVIDERLIB} PRIVATE
    Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Gui
    Qt${QT_DEFAULT_MAJOR_VERSION}::Widgets
)
