ENABLE_TESTING()

set(bigintmodunittest_bin_SRCS bigintmodunittest.cpp)

add_executable(bigintmodunittest ${bigintmodunittest_bin_SRCS})

target_link_qca_test_libraries(bigintmodunittest)

add_qca_test(bigintmodunittest "BigIntegerModularArithmetic")
