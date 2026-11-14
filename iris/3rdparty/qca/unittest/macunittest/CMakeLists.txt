ENABLE_TESTING()

set(macunittest_bin_SRCS macunittest.cpp)

add_executable( macunittest ${macunittest_bin_SRCS} )

target_link_qca_test_libraries(macunittest)

if(WITH_ossl_PLUGIN_INTERNAL)
  target_compile_definitions(macunittest PRIVATE QCA_TEST_EXPECT_OSSL_PLUGIN)
endif()

add_qca_test(macunittest "MessageAuthenticationCode")
