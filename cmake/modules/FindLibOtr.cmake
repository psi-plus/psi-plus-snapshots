if( LIBOTR_INCLUDE_DIR AND LIBOTR_LIBRARY )
	# in cache already
	set(LIBOTR_FIND_QUIETLY TRUE)
endif( LIBOTR_INCLUDE_DIR AND LIBOTR_LIBRARY )

if( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_LIBOTR QUIET libotr )
	if( PC_LIBOTR_FOUND )
		set( LIBOTR_DEFINITIONS ${PC_LIBOTR_CFLAGS} ${PC_LIBOTR_CFLAGS_OTHER} )
	endif( PC_LIBOTR_FOUND )
endif( UNIX AND NOT( APPLE OR CYGWIN ) )

set( LIBOTR_ROOT "" CACHE STRING "Path to libotr library" )

find_path(
	LIBOTR_INCLUDE_DIR libotr/privkey.h
	HINTS
	${LIBOTR_ROOT}/include
	${PC_LIBOTR_INCLUDEDIR}
	${PC_LIBOTR_INCLUDE_DIRS}
	PATH_SUFFIXES
	""
	libotr
)

find_library(
	LIBOTR_LIBRARY
	NAMES otr libotr
	HINTS 
	${PC_LIBOTR_LIBDIR}
	${PC_LIBOTR_LIBRARY_DIRS}
	${LIBOTR_ROOT}/lib
	${LIBOTR_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				LibOtr
				DEFAULT_MSG
				LIBOTR_LIBRARY
				LIBOTR_INCLUDE_DIR
)
if( LIBOTR_FOUND )
	set( LIBOTR_LIBRARIES ${LIBOTR_LIBRARY} )
	set( LIBOTR_INCLUDE_DIRS ${LIBOTR_INCLUDE_DIR} )
endif( LIBOTR_FOUND )

mark_as_advanced( LIBOTR_INCLUDE_DIR LIBOTR_LIBRARY )
