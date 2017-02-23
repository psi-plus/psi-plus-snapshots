if( ZLIB_INCLUDE_DIR AND ZLIB_LIBRARY )
	# in cache already
	set(ZLIB_FIND_QUIETLY TRUE)
endif( ZLIB_INCLUDE_DIR AND ZLIB_LIBRARY )

if( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_ZLIB QUIET zlib )
	if( PC_ZLIB_FOUND )
		set( ZLIB_DEFINITIONS ${PC_ZLIB_CFLAGS} ${PC_ZLIB_CFLAGS_OTHER} )
	endif( PC_ZLIB_FOUND )
endif( UNIX AND NOT( APPLE OR CYGWIN ) )

set( ZLIB_ROOT "" CACHE STRING "Path to libidn library" )

find_path(
	ZLIB_INCLUDE_DIR zlib.h
	HINTS
	${ZLIB_ROOT}/include
	${PC_ZLIB_INCLUDEDIR}
	${PC_ZLIB_INCLUDE_DIRS}
)

find_library(
	ZLIB_LIBRARY
	NAMES z lz zlib zlib1
	HINTS
	${PC_ZLIB_LIBDIR}
	${PC_ZLIB_LIBRARY_DIRS}
	${ZLIB_ROOT}/lib
	${ZLIB_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				ZLIB
				DEFAULT_MSG
				ZLIB_LIBRARY
				ZLIB_INCLUDE_DIR
)
if( ZLIB_FOUND )
	set( ZLIB_LIBRARIES ${ZLIB_LIBRARY} )
	set( ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR} )
endif( ZLIB_FOUND )

mark_as_advanced( ZLIB_INCLUDE_DIR ZLIB_LIBRARY )
