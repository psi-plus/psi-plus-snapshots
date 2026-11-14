enable_testing()

add_executable(saslscramunittest saslscramunittest.cpp)

target_link_qca_test_libraries(saslscramunittest)

add_qca_test(saslscramunittest "SaslScram")
