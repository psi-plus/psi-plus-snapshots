cmake_minimum_required(VERSION 3.19)

add_subdirectory(jdns)
if(BUILD_QJDNS)
    add_subdirectory(qjdns)
endif()
