cmake_minimum_required(VERSION 3.1.0)

unset(MOC_SOURCES)
unset(HEADERS)
unset(PLAIN_HEADERS)
unset(FORMS)
unset(SOURCES)
unset(PLAIN_SOURCES)
unset(UI_FORMS)

add_definitions(
	-DQHTTP_MEMORY_LOG=0
)
if(WIN32)
	add_definitions(
		-D_WINDOWS
		-DWIN32_LEAN_AND_MEAN
		-DNOMINMAX
		-DQHTTP_EXPORT
	)
endif()

include_directories(
	.
	qhttp/src
	http-parser
)

set(http_parser_srcs
	http-parser/http_parser.c
)

set(http_parser_hdrs
	http-parser/http_parser.h
)

set(qhttp_srcs
	qhttp/src/qhttpabstracts.cpp
	qhttp/src/qhttpserverconnection.cpp
	qhttp/src/qhttpserverrequest.cpp
	qhttp/src/qhttpserverresponse.cpp
	qhttp/src/qhttpserver.cpp
)

set(qhttp_hdrs
	qhttp/src/qhttpfwd.hpp
	qhttp/src/qhttpabstracts.hpp
	qhttp/src/qhttpserverconnection.hpp
	qhttp/src/qhttpserverrequest.hpp
	qhttp/src/qhttpserverresponse.hpp
	qhttp/src/qhttpserver.hpp
)

add_library(qhttp
	STATIC
	${http_parser_srcs}
	${http_parser_hdrs}
	${qhttp_srcs}
	${qhttp_hdrs}
)

set_property(TARGET qhttp PROPERTY CXX_STANDARD 14)
set_property(TARGET qhttp PROPERTY CXX_STANDARD_REQUIRED ON)

target_link_libraries(qhttp ${QT_LIBRARIES})
target_include_directories(qhttp
	PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
	${CMAKE_CURRENT_BINARY_DIR}/qhttp
)
