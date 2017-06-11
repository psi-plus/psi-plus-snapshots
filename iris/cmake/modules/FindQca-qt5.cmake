if (Qca-qt5_INCLUDE_DIR AND Qca-qt5_LIBRARY)
	# in cache already
	set(Qca-qt5_FIND_QUIETLY TRUE)
endif ()

set ( LIBINCS 
	qca.h
)

find_path(
	Qca-qt5_INCLUDE_DIR ${LIBINCS}
	HINTS
	${QCA_DIR}/include
	PATH_SUFFIXES
	qt5/Qca-qt5/QtCrypto
	Qca-qt5/QtCrypto
)

find_library(
	Qca-qt5_LIBRARY
	NAMES qca-qt5
	HINTS 
	${QCA_DIR}/lib
	${QCA_DIR}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
				Qca-qt5
				DEFAULT_MSG
				Qca-qt5_LIBRARY
				Qca-qt5_INCLUDE_DIR
)
if ( Qca-qt5_FOUND )
	set ( Qca-qt5_LIBRARIES ${Qca-qt5_LIBRARY} )
	set ( Qca-qt5_INCLUDE_DIRS ${Qca-qt5_INCLUDE_DIR} )
endif ( Qca-qt5_FOUND )

mark_as_advanced( Qca-qt5_INCLUDE_DIR Qca-qt5_LIBRARY )

