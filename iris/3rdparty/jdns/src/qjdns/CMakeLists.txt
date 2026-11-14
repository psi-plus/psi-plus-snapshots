cmake_minimum_required(VERSION 3.19)

set(qjdns_MOC_HDRS
    "${JDNS_INCLUDEDIR}/qjdns.h"
    "${JDNS_INCLUDEDIR}/qjdnsshared.h"
    "${JDNS_INCLUDEDIR}/qjdnsmulticast.h"
    "${JDNS_INCLUDEDIR}/qjdns_querytransport.h"
    "${JDNS_INCLUDEDIR}/qjdns_externaltransport.h"
    qjdns_p.h
    qjdns_datagramtransport_p.h
    qjdnsshared_p.h
)

set(qjdns_SRCS
    qjdns.cpp
    qjdns_sock.cpp
    qjdns_datagramtransport.cpp
    qjdns_tcptransport.cpp
    qjdns_httptransport.cpp
    qjdns_externaltransport.cpp
    qjdnsshared.cpp
    qjdnsmulticast.cpp
)

if(ANDROID)
    list(APPEND qjdns_SRCS qjdns_androidtransport.cpp)
endif()

set(qjdns_PUBLIC_HEADERS
    "${JDNS_INCLUDEDIR}/qjdns.h"
    "${JDNS_INCLUDEDIR}/qjdnsshared.h"
    "${JDNS_INCLUDEDIR}/qjdnsmulticast.h"
    "${JDNS_INCLUDEDIR}/qjdns_querytransport.h"
    "${JDNS_INCLUDEDIR}/qjdns_tcptransport.h"
    "${JDNS_INCLUDEDIR}/qjdns_httptransport.h"
    "${JDNS_INCLUDEDIR}/qjdns_externaltransport.h"
)

set(qjdns_HEADERS
    qjdns_sock.h
    qjdns_wire_p.h
)

add_library(qjdns
    ${qjdns_SRCS}
    ${qjdns_MOC_HDRS}
    ${qjdns_HEADERS}
    ${qjdns_PUBLIC_HEADERS}
)
add_library(QJDns::QJDns ALIAS qjdns)

set_target_properties(qjdns PROPERTIES AUTOMOC ON)
set_property(TARGET qjdns PROPERTY INTERFACE_QJDNS_QT_MAJOR_VERSION "${JDNS_QT_MAJOR_VERSION}")
set_property(TARGET qjdns APPEND PROPERTY COMPATIBLE_INTERFACE_STRING QJDNS_QT_MAJOR_VERSION)

if(QT_MAJ)
    set_target_properties(qjdns PROPERTIES OUTPUT_NAME "qjdns${QT_MAJ}")
endif()

target_include_directories(qjdns
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/jdns>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/jdns>
)

target_link_libraries(qjdns
    PUBLIC
        JDns::JDns
        ${QJDNS_QT_TARGETS}
)

if(WIN32)
    # qjdns_sock.cpp calls Winsock directly; a shared jdns does not re-export it.
    target_link_libraries(qjdns PRIVATE ws2_32)
endif()

if(ANDROID)
    target_link_libraries(qjdns PRIVATE ${CMAKE_DL_LIBS})
endif()

if(NOT ANDROID)
    set_target_properties(qjdns PROPERTIES
        VERSION "${JDNS_LIB_VERSION_STRING}"
        SOVERSION "${JDNS_LIB_MAJOR_VERSION}"
    )
endif()

set_target_properties(qjdns PROPERTIES
    DEFINE_SYMBOL JDNS_MAKEDLL
    PUBLIC_HEADER "${qjdns_PUBLIC_HEADERS}"
)

if(JDNS_ENABLE_INSTALL)
    install(TARGETS qjdns
        EXPORT qjdns-export
        LIBRARY DESTINATION "${LIB_INSTALL_DIR}"
        RUNTIME DESTINATION "${BIN_INSTALL_DIR}"
        ARCHIVE DESTINATION "${LIB_INSTALL_DIR}"
        PUBLIC_HEADER DESTINATION "${INCLUDE_INSTALL_DIR}/jdns"
        INCLUDES DESTINATION "${INCLUDE_INSTALL_DIR}/jdns"
    )

    if(MSVC AND BUILD_SHARED_LIBS)
        install(FILES "$<TARGET_PDB_FILE:qjdns>"
            DESTINATION "${BIN_INSTALL_DIR}"
            CONFIGURATIONS Debug RelWithDebInfo
            OPTIONAL
        )
    endif()
endif()
