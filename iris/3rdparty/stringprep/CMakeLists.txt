cmake_minimum_required(VERSION 3.10.0)

project(stringprep
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Core REQUIRED)

add_library(stringprep STATIC
    stringprep.cpp
    profiles.cpp
    rfc3454.cpp
)

target_compile_definitions(stringprep PRIVATE QSTRINGPREP_BUILDING)

if(MSVC)
  target_compile_definitions(stringprep PRIVATE _CRT_SECURE_NO_WARNINGS _GENERATED_STDINT_H)
endif()

target_link_libraries(stringprep PUBLIC Qt5::Core)
target_include_directories(stringprep PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
