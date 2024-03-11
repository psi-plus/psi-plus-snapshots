project(ICETunnel
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)

add_executable(icetunnel main.cpp)

target_link_libraries(icetunnel PRIVATE iris Qt::Core Qt::Network)
target_include_directories(icetunnel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/iris
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(icetunnel PRIVATE QCA_STATIC)
