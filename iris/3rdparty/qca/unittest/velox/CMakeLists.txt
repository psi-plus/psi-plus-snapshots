find_package(Qt${QT_MAJOR_VERSION} COMPONENTS Network REQUIRED)

ENABLE_TESTING()

set(veloxunittest_bin_SRCS veloxunittest.cpp)

add_executable(veloxunittest ${veloxunittest_bin_SRCS})

target_link_qca_test_libraries(veloxunittest)
target_link_libraries(veloxunittest Qt${QT_MAJOR_VERSION}::Network)

set(VELOX_CERTIFICATES
    root.crt
    alice.crt
    alice.key
    bob.crt
    bob.key
    carol.crt
    carol.key
    dave.crt
    dave.key
    mallory.crt
    mallory.key
    ivan.crt
    ivan.key
    default.crt
    default.key
)

file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/velox-certs")
foreach(CERTIFICATE_FILE IN LISTS VELOX_CERTIFICATES)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/certs/${CERTIFICATE_FILE}"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/velox-certs/${CERTIFICATE_FILE}"
        COPYONLY
    )
endforeach()

add_qca_test(veloxunittest "ServerNameIndication")
