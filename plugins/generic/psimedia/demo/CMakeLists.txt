cmake_minimum_required(VERSION 3.10.0)

project(psimedia-demo LANGUAGES CXX)

add_definitions(-DDEBUG_POSTFIX=\"\")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(APPLE)
    add_definitions(-DDEBUG_POSTFIX=\"_debug\")
  elseif(WIN32)
    add_definitions(-DDEBUG_POSTFIX=\"d\")
  endif()
  add_definitions(-DPLUGIN_INSTALL_PATH_DEBUG=\"${CMAKE_BINARY_DIR}/psimedia\")
endif()

find_package(Qt5 COMPONENTS Core Widgets Gui Network REQUIRED)

set(CMAKE_AUTOMOC ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../psimedia
)

add_definitions(-DPLUGIN_INSTALL_PATH=\"${LIB_INSTALL_DIR}\")

set(HEADERS
    main.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../psimedia/psimedia.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../psimedia/psimedia_p.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../psimedia/psimediaprovider.h
)

set(SOURCES
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../psimedia/psimedia.cpp
)
set(FORMS config.ui mainwin.ui)

qt5_wrap_ui(UI_SOURCES ${FORMS})

add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${UI_SOURCES})

if(NOT "${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
    add_dependencies( ${PROJECT_NAME} gstprovider )
endif()

target_link_libraries(${PROJECT_NAME} Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network)

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${BIN_INSTALL_DIR})
