ENABLE_TESTING()

add_executable(dtlssrtpunittest dtlssrtpunittest.cpp)

target_link_qca_test_libraries(dtlssrtpunittest)

file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dtlssrtp-certs")
foreach(CERTIFICATE_FILE IN ITEMS root.crt default.crt default.key)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../velox/certs/${CERTIFICATE_FILE}"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/dtlssrtp-certs/${CERTIFICATE_FILE}"
        COPYONLY
    )
endforeach()

add_qca_test(dtlssrtpunittest "DTLSSRTP")
