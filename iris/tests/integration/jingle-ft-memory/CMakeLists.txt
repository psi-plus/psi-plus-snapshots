cmake_minimum_required(VERSION 3.19)
project(iris_jingle_ft_memory_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(QT_DEFAULT_MAJOR_VERSION 6)

option(IRIS_FT_MEMORY_SANITIZE "Instrument Iris and the repeated FT probe with ASan/UBSan" OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Xml)

if(IRIS_FT_MEMORY_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -fsanitize=address,undefined
        -fno-sanitize=enum
        -fno-omit-frame-pointer
        -O1
        -g
    )
    add_link_options(
        -fsanitize=address,undefined
        -fno-sanitize=enum
        -fno-omit-frame-pointer
    )
    add_compile_definitions(IRIS_FT_MEMORY_ASAN_PROFILE=1)
endif()

# Build only Iris and this integration probe. Keeping it independent from Psi
# makes repeated leak diagnostics cheap enough to run while iterating on Iris.
set(IRIS_VERSION 1.1 CACHE STRING "" FORCE)
set(IRIS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(IRIS_ENABLE_JINGLE_SCTP ON CACHE BOOL "" FORCE)
set(IRIS_ENABLE_OMEMO OFF CACHE BOOL "" FORCE)
set(IRIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IRIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(IRIS_BUNDLED_USRSCTP OFF CACHE BOOL "" FORCE)
set(IRIS_BUNDLED_QCA OFF CACHE BOOL "" FORCE)
set(IRIS_SYSTEM_QCA 3 CACHE STRING "" FORCE)
set(USE_QT6 ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." iris)

add_executable(iris_jingle_ft_memory_probe ft_memory_probe.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(iris_jingle_ft_memory_probe PRIVATE
        "SHELL:-include iris/xmpp-im/jingle-s5b.h"
        "SHELL:-include iris/xmpp-im/jingle-ibb.h"
    )
endif()
target_link_libraries(iris_jingle_ft_memory_probe PRIVATE
    Iris::Iris
    Qt6::Core
    Qt6::Network
    Qt6::Xml
)
