cmake_minimum_required(VERSION 3.19)

set(jdns_SRCS
    jdns.c
    jdns_multicast.c
    jdns_mdnsd.c
    jdns_packet.c
    jdns_sys.c
    jdns_util.c
)

set(jdns_PUBLIC_HEADERS
    "${JDNS_INCLUDEDIR}/jdns.h"
    "${JDNS_INCLUDEDIR}/jdns_export.h"
)

set(jdns_HEADERS
    jdns_packet.h
    jdns_mdnsd.h
    jdns_p.h
    jdns_core_p.h
)

add_library(jdns ${jdns_SRCS} ${jdns_HEADERS} ${jdns_PUBLIC_HEADERS})
add_library(JDns::JDns ALIAS jdns)

target_include_directories(jdns
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/jdns>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/jdns>
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(jdns PUBLIC JDNS_STATIC)
endif()

if(WIN32)
    # MinGW import library names are case-sensitive on Linux cross-build hosts.
    target_link_libraries(jdns PRIVATE ws2_32 advapi32)
endif()

if(NOT ANDROID)
    set_target_properties(jdns PROPERTIES
        VERSION "${JDNS_LIB_VERSION_STRING}"
        SOVERSION "${JDNS_LIB_MAJOR_VERSION}"
    )
endif()

set_target_properties(jdns PROPERTIES
    DEFINE_SYMBOL JDNS_MAKEDLL
    PUBLIC_HEADER "${jdns_PUBLIC_HEADERS}"
)

if(JDNS_ENABLE_INSTALL)
    install(TARGETS jdns
        EXPORT jdns-export
        LIBRARY DESTINATION "${LIB_INSTALL_DIR}"
        RUNTIME DESTINATION "${BIN_INSTALL_DIR}"
        ARCHIVE DESTINATION "${LIB_INSTALL_DIR}"
        PUBLIC_HEADER DESTINATION "${INCLUDE_INSTALL_DIR}/jdns"
        INCLUDES DESTINATION "${INCLUDE_INSTALL_DIR}/jdns"
    )

    if(MSVC AND BUILD_SHARED_LIBS)
        install(FILES "$<TARGET_PDB_FILE:jdns>"
            DESTINATION "${BIN_INSTALL_DIR}"
            CONFIGURATIONS Debug RelWithDebInfo
            OPTIONAL
        )
    endif()
endif()
