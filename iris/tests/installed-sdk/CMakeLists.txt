cmake_minimum_required(VERSION 3.11)
project(iris_installed_sdk_test LANGUAGES CXX)

# Deliberately independent of the Iris build/source tree.
find_package(Iris CONFIG REQUIRED)
add_executable(iris_installed_sdk_test main.cpp)
target_compile_features(iris_installed_sdk_test PRIVATE cxx_std_20)
target_link_libraries(iris_installed_sdk_test PRIVATE Iris::Iris)
enable_testing()
add_test(NAME iris_installed_sdk COMMAND iris_installed_sdk_test)
