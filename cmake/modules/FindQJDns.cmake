if (QJDns_INCLUDE_DIR AND QJDns_LIBRARY)
	# in cache already
	set(QJDns_FIND_QUIETLY TRUE)
endif ()

if ( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_QJDns QUIET jdns )
	set ( QJDns_DEFINITIONS 
		${PC_QJDns_CFLAGS}
		${PC_QJDns_CFLAGS_OTHER}
	)
endif ( UNIX AND NOT( APPLE OR CYGWIN ) )

set ( LIBINCS 
	qjdns.h
)

find_path(
	QJDns_INCLUDE_DIR ${LIBINCS}
	HINTS
	${QJDNS_DIR}/include
	${PC_QJDns_INCLUDEDIR}
	${PC_QJDns_INCLUDE_DIRS}
	PATH_SUFFIXES
	""
	if( NOT WIN32 )
	jdns
    endif( NOT WIN32 )
)

find_library(
	QJDns_LIBRARY
	NAMES qjdns
	HINTS 
	${PC_QJDns_LIBDIR}
	${PC_QJDns_LIBRARY_DIRS}
	${QJDNS_DIR}/lib
	${QJDNS_DIR}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				QJDns
				DEFAULT_MSG
				QJDns_LIBRARY
				QJDns_INCLUDE_DIR
)
if ( QJDns_FOUND )
	set ( QJDns_LIBRARIES ${QJDns_LIBRARY} )
	set ( QJDns_INCLUDE_DIRS ${QJDns_INCLUDE_DIR} )
endif ( QJDns_FOUND )

mark_as_advanced( QJDns_INCLUDE_DIR QJDns_LIBRARY )

