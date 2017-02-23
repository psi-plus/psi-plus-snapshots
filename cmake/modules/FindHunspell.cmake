if (HUNSPELL_INCLUDE_DIR AND HUNSPELL_LIBRARY)
	# in cache already
	set(HUNSPELL_FIND_QUIETLY TRUE)
endif ()

if ( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_HUNSPELL QUIET hunspell )
	set ( HUNSPELL_DEFINITIONS 
		${PC_HUNSPELL_CFLAGS}
		${PC_HUNSPELL_CFLAGS_OTHER}
	)
endif ( UNIX AND NOT( APPLE OR CYGWIN ) )

set ( LIBINCS 
	hunspell.hxx
)

find_path(
	HUNSPELL_INCLUDE_DIR ${LIBINCS}
	HINTS
	${HUNSPELL_ROOT}/include
	${PC_HUNSPELL_INCLUDEDIR}
	${PC_HUNSPELL_INCLUDE_DIRS}
	PATH_SUFFIXES
	""
	if ( NOT ${WIN32} )
	hunspell
	endif ( NOT ${WIN32} )
)

find_library(
	HUNSPELL_LIBRARY
	NAMES hunspell hunspell-1.3 libhunspell
	HINTS 
	${PC_HUNSPELL_LIBDIR}
	${PC_HUNSPELL_LIBRARY_DIRS}
	${HUNSPELL_ROOT}/lib
	${HUNSPELL_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				Hunspell
				DEFAULT_MSG
				HUNSPELL_LIBRARY
				HUNSPELL_INCLUDE_DIR
)
if ( HUNSPELL_FOUND )
	set ( HUNSPELL_LIBRARIES ${HUNSPELL_LIBRARY} )
	set ( HUNSPELL_INCLUDE_DIRS ${HUNSPELL_INCLUDE_DIR} )
endif ( HUNSPELL_FOUND )

mark_as_advanced( HUNSPELL_INCLUDE_DIR HUNSPELL_LIBRARY )

