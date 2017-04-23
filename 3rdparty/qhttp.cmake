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
	${PROJECT_SOURCE_DIR}/3rdparty
	${PROJECT_SOURCE_DIR}/3rdparty/http-parser
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --std=c++14")

set(http_parser_srcs
	${PROJECT_SOURCE_DIR}/3rdparty/http-parser/http_parser.c
)

set(http_parser_hdrs
	${PROJECT_SOURCE_DIR}/3rdparty/http-parser/http_parser.h
)

set(qhttp_srcs
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpabstracts.cpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserverconnection.cpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserverrequest.cpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserverresponse.cpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserver.cpp
)

set(qhttp_hdrs
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpfwd.hpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpabstracts.hpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserverconnection.hpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserverrequest.hpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserverresponse.hpp
	${PROJECT_SOURCE_DIR}/3rdparty/qhttp/src/qhttpserver.hpp
)

list(APPEND PLAIN_SOURCES
	${http_parser_srcs}
	${qhttp_srcs}
)

list(APPEND PLAIN_HEADERS
	${http_parser_hdrs}
	${qhttp_hdrs}
)
