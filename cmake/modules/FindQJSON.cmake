if (QJSON_INCLUDE_DIR AND QJSON_LIBRARY)
	# in cache already
	set(QJSON_FIND_QUIETLY TRUE)
endif ()

if ( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_QJSON QUIET QJson )
	set ( QJSON_DEFINITIONS 
		${PC_QJSON_CFLAGS}
		${PC_QJSON_CFLAGS_OTHER}
	)
endif ( UNIX AND NOT( APPLE OR CYGWIN ) )

set ( LIBINCS 
	parser.h
)

find_path(
	QJSON_INCLUDE_DIR ${LIBINCS}
	HINTS
	${QJSON_ROOT}/include
	${PC_QJSON_INCLUDEDIR}
	${PC_QJSON_INCLUDE_DIRS}
	PATH_SUFFIXES
	""
	if ( NOT ${WIN32} )
	qjson
	endif ( NOT ${WIN32} )
)

find_library(
	QJSON_LIBRARY
	NAMES qjson
	HINTS 
	${PC_QJSON_LIBDIR}
	${PC_QJSON_LIBRARY_DIRS}
	${QJSON_ROOT}/lib
	${QJSON_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				QJson
				DEFAULT_MSG
				QJSON_LIBRARY
				QJSON_INCLUDE_DIR
)
if ( QJSON_FOUND )
	set ( QJSON_LIBRARIES ${QJSON_LIBRARY} )
	set ( QJSON_INCLUDE_DIRS ${QJSON_INCLUDE_DIR} )
endif ( QJSON_FOUND )

mark_as_advanced( QJSON_INCLUDE_DIR QJSON_LIBRARY )

