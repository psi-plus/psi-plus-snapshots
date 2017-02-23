if( IDN_INCLUDE_DIR AND IDN_LIBRARY )
	# in cache already
	set(IDN_FIND_QUIETLY TRUE)
endif( IDN_INCLUDE_DIR AND IDN_LIBRARY )

if( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_IDN QUIET libidn )
	if( PC_IDN_FOUND )
		set( IDN_DEFINITIONS ${PC_IDN_CFLAGS} ${PC_IDN_CFLAGS_OTHER} )
	endif( PC_IDN_FOUND )
endif( UNIX AND NOT( APPLE OR CYGWIN ) )

set( IDN_ROOT "" CACHE STRING "Path to libidn library" )

find_path(
	IDN_INCLUDE_DIR idna.h
	HINTS
	${IDN_ROOT}/include
	${PC_IDN_INCLUDEDIR}
	${PC_IDN_INCLUDE_DIRS}
)

find_library(
	IDN_LIBRARY
	NAMES idn libidn idn-11 libidn-11
	HINTS
	${PC_IDN_LIBDIR}
	${PC_IDN_LIBRARY_DIRS}
	${IDN_ROOT}/lib
	${IDN_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				IDN
				DEFAULT_MSG
				IDN_LIBRARY
				IDN_INCLUDE_DIR
)
if( IDN_FOUND )
	set( IDN_LIBRARIES ${IDN_LIBRARY} )
	set( IDN_INCLUDE_DIRS ${IDN_INCLUDE_DIR} )
endif( IDN_FOUND )

mark_as_advanced( IDN_INCLUDE_DIR IDN_LIBRARY )
