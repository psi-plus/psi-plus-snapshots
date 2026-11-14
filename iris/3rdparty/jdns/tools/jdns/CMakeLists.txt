cmake_minimum_required(VERSION 3.19)

set(jdns_tool_MOC_HDRS
    main.h
)

set(jdns_tool_SRCS
    main.cpp
)

add_executable(jdns-tool
    ${jdns_tool_SRCS}
    ${jdns_tool_MOC_HDRS}
)

set_target_properties(jdns-tool PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME jdns
)

target_link_libraries(jdns-tool PRIVATE QJDns::QJDns)

if(JDNS_ENABLE_INSTALL)
    install(TARGETS jdns-tool
        RUNTIME DESTINATION "${BIN_INSTALL_DIR}"
    )

    if(MSVC)
        install(FILES "$<TARGET_PDB_FILE:jdns-tool>"
            DESTINATION "${BIN_INSTALL_DIR}"
            CONFIGURATIONS Debug RelWithDebInfo
            OPTIONAL
        )
    endif()
endif()
