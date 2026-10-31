find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Network)

add_executable(net_remotedns_test remotedns.cpp)
target_compile_definitions(net_remotedns_test PRIVATE QT_STATICPLUGIN)
target_link_libraries(net_remotedns_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Network)
add_test(NAME net_remotedns COMMAND net_remotedns_test)
set_tests_properties(net_remotedns PROPERTIES TIMEOUT 30)
