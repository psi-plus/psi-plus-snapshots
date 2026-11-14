cmake_minimum_required(VERSION 3.19)

add_executable(jdns-native-async-test native_async.c)
target_link_libraries(jdns-native-async-test PRIVATE JDns::JDns)
add_test(NAME jdns-native-async COMMAND jdns-native-async-test)

add_executable(jdns-multicast-test multicast.c)
target_link_libraries(jdns-multicast-test PRIVATE JDns::JDns)
add_test(NAME jdns-multicast COMMAND jdns-multicast-test)

if(BUILD_QJDNS)
find_package(Qt${JDNS_QT_MAJOR_VERSION} REQUIRED COMPONENTS Test)

add_executable(qjdns-transport-test transports.cpp)
set_target_properties(qjdns-transport-test PROPERTIES AUTOMOC ON)
target_link_libraries(qjdns-transport-test PRIVATE
    QJDns::QJDns
    Qt${JDNS_QT_MAJOR_VERSION}::Test
    Qt${JDNS_QT_MAJOR_VERSION}::Network
)
add_test(NAME qjdns-transports COMMAND qjdns-transport-test)
set_tests_properties(qjdns-transports PROPERTIES TIMEOUT 30)

add_executable(qjdns-multicast-test multicast.cpp)
set_target_properties(qjdns-multicast-test PROPERTIES AUTOMOC ON)
target_link_libraries(qjdns-multicast-test PRIVATE QJDns::QJDns Qt${JDNS_QT_MAJOR_VERSION}::Test)
add_test(NAME qjdns-multicast COMMAND qjdns-multicast-test)
set_tests_properties(qjdns-multicast PROPERTIES TIMEOUT 90)
endif()
