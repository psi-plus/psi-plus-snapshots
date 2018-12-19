cmake_minimum_required(VERSION 3.1.0)
project(qite)

include(GNUInstallDirs)

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
# Instruct CMake to run moc automatically when needed
set(CMAKE_AUTOMOC ON)

# Find the QtWidgets library
find_package(Qt5 REQUIRED Widgets Multimedia)

include(libqite.cmake)

add_library(qite STATIC ${qite_SOURCES})
set_property(TARGET qite PROPERTY CXX_STANDARD 14)
target_link_libraries(qite Qt5::Widgets Qt5::Multimedia)

install(TARGETS qite DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${qite_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qite)
