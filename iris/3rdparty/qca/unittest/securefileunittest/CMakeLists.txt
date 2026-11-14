ENABLE_TESTING()

set(securefileunittest_bin_SRCS securefileunittest.cpp)

add_executable(securefileunittest ${securefileunittest_bin_SRCS})
target_link_qca_test_libraries(securefileunittest)

foreach(test_case roundTrip overwrite emptyFile sizeLimit rejectDirectory)
    set(test_name "SecureFile.${test_case}")
    add_test(NAME "${test_name}"
             WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
             COMMAND securefileunittest ${test_case} -v2)
    set_tests_properties("${test_name}" PROPERTIES
                         ENVIRONMENT "QCA_PLUGIN_PATH=${CMAKE_BINARY_DIR}/lib/${QCA_LIB_NAME}")
endforeach()

if(NOT WIN32)
    foreach(test_case rejectSymlink privatePermissions)
        set(test_name "SecureFile.${test_case}")
        add_test(NAME "${test_name}"
                 WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                 COMMAND securefileunittest ${test_case} -v2)
        set_tests_properties("${test_name}" PROPERTIES
                             ENVIRONMENT "QCA_PLUGIN_PATH=${CMAKE_BINARY_DIR}/lib/${QCA_LIB_NAME}")
    endforeach()
endif()
