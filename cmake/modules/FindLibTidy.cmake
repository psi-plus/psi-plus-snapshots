if( LIBTIDY_INCLUDE_DIR AND LIBTIDY_LIBRARY )
	# in cache already
	set(LIBTIDY_FIND_QUIETLY TRUE)
endif( LIBTIDY_INCLUDE_DIR AND LIBTIDY_LIBRARY )

if( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_LIBTIDY QUIET libtidy )
	if( PC_LIBTIDY_FOUND )
		set( LIBTIDY_DEFINITIONS ${PC_LIBTIDY_CFLAGS} ${PC_LIBTIDY_CFLAGS_OTHER} )
	endif( PC_LIBTIDY_FOUND )
endif( UNIX AND NOT( APPLE OR CYGWIN ) )

set( LIBTIDY_ROOT "" CACHE STRING "Path to libtidy library" )

find_path(
	LIBTIDY_INCLUDE_DIR tidy.h
	HINTS
	${PC_LIBTIDY_INCLUDEDIR}
	${PC_LIBTIDY_INCLUDE_DIRS}
	PATHS
	${LIBTIDY_ROOT}/include
	PATH_SUFFIXES
	""
	tidy
)

if( EXISTS "${LIBTIDY_INCLUDE_DIR}/buffio.h" OR (EXISTS "${LIBTIDY_INCLUDE_DIR}/tidy/buffio.h") )
	set( LIBTIDY_DEFINITIONS "${LIBTIDY_DEFINITIONS} -DLEGACY_TIDY" )
endif()

find_library(
	LIBTIDY_LIBRARY
	NAMES tidy libtidy libtidy-0-99-0 tidy-0-99-0
	HINTS 
	${PC_LIBTIDY_LIBDIR}
	${PC_LIBTIDY_LIBRARY_DIRS}
	${LIBTIDY_ROOT}/lib
	${LIBTIDY_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				LibTidy
				DEFAULT_MSG
				LIBTIDY_LIBRARY
				LIBTIDY_INCLUDE_DIR
)
if( LIBTIDY_FOUND )
	set( LIBTIDY_LIBRARIES ${LIBTIDY_LIBRARY} )
	set( LIBTIDY_INCLUDE_DIRS ${LIBTIDY_INCLUDE_DIR} )
endif( LIBTIDY_FOUND )

mark_as_advanced( LIBTIDY_INCLUDE_DIR LIBTIDY_LIBRARY )
