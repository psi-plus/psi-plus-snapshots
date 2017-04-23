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

list(APPEND PLAIN_SOURCES
	${http_parser_srcs}
	${qhttp_srcs}
)

list(APPEND PLAIN_HEADERS
	${http_parser_hdrs}
	${qhttp_hdrs}
)
