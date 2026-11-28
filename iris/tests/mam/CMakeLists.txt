cmake_minimum_required(VERSION 3.16)

find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Network Xml)
add_executable(iris_mamtask_test mamtask.cpp)
target_link_libraries(iris_mamtask_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME iris_mamtask COMMAND iris_mamtask_test)
set_tests_properties(iris_mamtask PROPERTIES TIMEOUT 10)

if(IRIS_ENABLE_OMEMO)
    add_executable(iris_mam_omemo_test omemo.cpp)
    target_link_libraries(iris_mam_omemo_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
        Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
    add_test(NAME iris_mam_omemo COMMAND iris_mam_omemo_test)
    set_tests_properties(iris_mam_omemo PROPERTIES TIMEOUT 40)
endif()
