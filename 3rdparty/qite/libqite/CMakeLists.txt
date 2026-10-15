cmake_minimum_required(VERSION 3.10)
project(qite LANGUAGES CXX)

set(QT_DEFAULT_MAJOR_VERSION 5 CACHE STRING "Qt major version")

include(GNUInstallDirs)

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
# Instruct CMake to run moc automatically when needed
set(CMAKE_AUTOMOC ON)

# Find the QtWidgets library
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Widgets Multimedia)

include(libqite.cmake)

add_library(qite STATIC ${qite_SOURCES})
set_property(TARGET qite PROPERTY CXX_STANDARD 17)
target_link_libraries(qite PUBLIC Qt${QT_DEFAULT_MAJOR_VERSION}::Widgets Qt${QT_DEFAULT_MAJOR_VERSION}::Multimedia)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(../tests tests)
endif()

install(TARGETS qite DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${qite_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qite)
