cmake_minimum_required(VERSION 3.2.0)

project(stringprep LANGUAGES CXX)


set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(CMAKE_VERSION VERSION_LESS "3.7.0")
    set(CMAKE_INCLUDE_CURRENT_DIR ON)
endif()

find_package(Qt5 COMPONENTS Core REQUIRED)

add_library(stringprep STATIC
  stringprep.cpp
  profiles.cpp
  rfc3454.cpp
)
target_compile_definitions(stringprep PRIVATE QSTRINGPREP_BUILDING)
target_include_directories(stringprep PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
  target_compile_definitions(stringprep PRIVATE _CRT_SECURE_NO_WARNINGS _GENERATED_STDINT_H)
endif()

target_link_libraries(stringprep Qt5::Core)
