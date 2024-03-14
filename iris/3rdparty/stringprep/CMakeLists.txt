cmake_minimum_required(VERSION 3.10.0)

project(stringprep
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(QT_DEFAULT_MAJOR_VERSION LESS 6)
    find_package(Qt5 5.10 REQUIRED COMPONENTS Core)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core)
endif()

add_library(stringprep STATIC
    stringprep.cpp
    profiles.cpp
    rfc3454.cpp
    qstringprep.h
)

target_compile_definitions(stringprep PRIVATE QSTRINGPREP_BUILDING)

if(MSVC)
  target_compile_definitions(stringprep PRIVATE _CRT_SECURE_NO_WARNINGS _GENERATED_STDINT_H)
endif()

if(QT_DEFAULT_MAJOR_VERSION LESS 6)
    target_link_libraries(stringprep PUBLIC Qt5::Core)
else()
    target_link_libraries(stringprep PUBLIC Qt6::Core)
endif()
target_include_directories(stringprep PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
