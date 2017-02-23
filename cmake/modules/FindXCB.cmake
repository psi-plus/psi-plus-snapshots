if (XCB_INCLUDE_DIR AND XCB_LIBRARY)
	# in cache already
	set(XCB_FIND_QUIETLY TRUE)
endif ()

if ( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_XCB QUIET xcb )
	set ( XCB_DEFINITIONS 
		${PC_XCB_CFLAGS}
		${PC_XCB_CFLAGS_OTHER}
	)
endif ( UNIX AND NOT( APPLE OR CYGWIN ) )

set ( LIBINCS 
	xcb.h
)

find_path(
	XCB_INCLUDE_DIR ${LIBINCS}
	HINTS
	${XCB_ROOT}/include
	${PC_XCB_INCLUDEDIR}
	${PC_XCB_INCLUDE_DIRS}
	PATH_SUFFIXES
	"xcb"
)

find_library(
	XCB_LIBRARY
	NAMES xcb
	HINTS 
	${PC_XCB_LIBDIR}
	${PC_XCB_LIBRARY_DIRS}
	${XCB_ROOT}/lib
	${XCB_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				XCB
				DEFAULT_MSG
				XCB_LIBRARY
				XCB_INCLUDE_DIR
)
if ( XCB_FOUND )
	set ( XCB_LIBRARIES ${XCB_LIBRARY} )
	set ( XCB_INCLUDE_DIRS ${XCB_INCLUDE_DIR} )
endif ( XCB_FOUND )

mark_as_advanced( XCB_INCLUDE_DIR XCB_LIBRARY )

