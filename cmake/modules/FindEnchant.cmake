if (Enchant_INCLUDE_DIR AND Enchant_LIBRARY)
	# in cache already
	set(Enchant_FIND_QUIETLY TRUE)
endif ()

if ( UNIX AND NOT( APPLE OR CYGWIN ) )
	find_package( PkgConfig QUIET )
	pkg_check_modules( PC_Enchant QUIET enchant )
	set ( Enchant_DEFINITIONS 
		${PC_Enchant_CFLAGS}
		${PC_Enchant_CFLAGS_OTHER}
	)
endif ( UNIX AND NOT( APPLE OR CYGWIN ) )

set ( LIBINCS 
	enchant.h
)

find_path(
	Enchant_INCLUDE_DIR ${LIBINCS}
	HINTS
	${Enchant_ROOT}/include
	${PC_Enchant_INCLUDEDIR}
	${PC_Enchant_INCLUDE_DIRS}
	PATH_SUFFIXES
	""
	if ( NOT ${WIN32} )
	enchant
	endif ( NOT ${WIN32} )
)

find_library(
	Enchant_LIBRARY
	NAMES enchant
	HINTS 
	${PC_Enchant_LIBDIR}
	${PC_Enchant_LIBRARY_DIRS}
	${Enchant_ROOT}/lib
	${Enchant_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				Enchant
				DEFAULT_MSG
				Enchant_LIBRARY
				Enchant_INCLUDE_DIR
)
if ( Enchant_FOUND )
	set ( Enchant_LIBRARIES ${Enchant_LIBRARY} )
	set ( Enchant_INCLUDE_DIRS ${Enchant_INCLUDE_DIR} )
endif ( Enchant_FOUND )

mark_as_advanced( Enchant_INCLUDE_DIR Enchant_LIBRARY )

