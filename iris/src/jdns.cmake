add_definitions(-DJDNS_STATIC)
set(QJDns_LIBRARY qjdns)
include_directories(
    src/jdns/include/jdns
)
set(jdns_SRCS
    src/jdns/src/jdns/jdns.c
    src/jdns/src/jdns/jdns_mdnsd.c
    src/jdns/src/jdns/jdns_packet.c
    src/jdns/src/jdns/jdns_sys.c
    src/jdns/src/jdns/jdns_util.c
)
set(jdns_PUBLIC_HEADERS
    src/jdns/include/jdns/jdns.h
    src/jdns/include/jdns/jdns_export.h
)
set(jdns_HEADERS
    src/jdns/src/jdns/jdns_packet.h
    src/jdns/src/jdns/jdns_mdnsd.h
    src/jdns/src/jdns/jdns_p.h
)

add_library(jdns STATIC ${jdns_SRCS} ${jdns_HEADERS} ${jdns_PUBLIC_HEADERS})

if(WIN32)
    target_link_libraries(jdns ws2_32 advapi32)
endif()
set(qjdns_MOC_HDRS
    src/jdns/include/jdns/qjdns.h
    src/jdns/include/jdns/qjdnsshared.h
    src/jdns/src/qjdns/qjdns_p.h
    src/jdns/src/qjdns/qjdnsshared_p.h
)

set(qjdns_SRCS
    src/jdns/src/qjdns/qjdns.cpp
    src/jdns/src/qjdns/qjdns_sock.cpp
    src/jdns/src/qjdns/qjdnsshared.cpp
)

set(qjdns_PUBLIC_HEADERS
    src/jdns/include/jdns/qjdns.h
    src/jdns/include/jdns/qjdnsshared.h
)
set(qjdns_HEADERS
    src/jdns/src/qjdns/qjdns_sock.h
)

add_library(${QJDns_LIBRARY} STATIC ${qjdns_SRCS} ${qjdns_MOC_SRCS} ${qjdns_MOC_HDRS} ${qjdns_PUBLIC_HEADERS})

target_link_libraries(${QJDns_LIBRARY} Qt5::Core Qt5::Network)
target_link_libraries(${QJDns_LIBRARY} jdns) 
