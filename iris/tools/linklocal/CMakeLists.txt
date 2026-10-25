add_executable(iris-linklocal main.cpp)
target_link_libraries(iris-linklocal PRIVATE iris QJDns::QJDns)

if(IRIS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME linklocal_mdns_tcp
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/linklocal/mdns_tcp.py
                    $<TARGET_FILE:iris-linklocal>)
        set_tests_properties(linklocal_mdns_tcp PROPERTIES TIMEOUT 45)
    endif()
endif()
