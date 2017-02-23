if (Qca_INCLUDE_DIR AND Qca_LIBRARY)
	# in cache already
	set(Qca_FIND_QUIETLY TRUE)
endif ()

set ( LIBINCS 
	qca.h
)

find_path(
	Qca_INCLUDE_DIR ${LIBINCS}
	HINTS
	${QCA_DIR}/include
	PATH_SUFFIXES
	QtCrypto
)

find_library(
	Qca_LIBRARY
	NAMES qca
	HINTS 
	${QCA_DIR}/lib
	${QCA_DIR}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				Qca
				DEFAULT_MSG
				Qca_LIBRARY
				Qca_INCLUDE_DIR
)
if ( Qca_FOUND )
	set ( Qca_LIBRARIES ${Qca_LIBRARY} )
	set ( Qca_INCLUDE_DIRS ${Qca_INCLUDE_DIR} )
endif ( Qca_FOUND )

mark_as_advanced( Qca_INCLUDE_DIR Qca_LIBRARY )

