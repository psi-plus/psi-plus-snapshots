cmake_minimum_required(VERSION 3.19)
project(iris_jinglepub_prosody LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_DEFAULT_MAJOR_VERSION 6)

find_package(Qt6 REQUIRED COMPONENTS Core Network Xml)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(IRIS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(IRIS_ENABLE_JINGLE_SCTP OFF CACHE BOOL "" FORCE)
set(IRIS_ENABLE_OMEMO OFF CACHE BOOL "" FORCE)
set(IRIS_BUNDLED_QCA OFF CACHE BOOL "" FORCE)
set(IRIS_SYSTEM_QCA 3 CACHE STRING "" FORCE)
set(USE_QT6 ON CACHE BOOL "" FORCE)
set(IRIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." iris)

add_executable(iris_jinglepub_prosody_client client.cpp)
target_link_libraries(iris_jinglepub_prosody_client PRIVATE
    Iris::Iris
    Qt6::Core
    Qt6::Network
    Qt6::Xml
)
