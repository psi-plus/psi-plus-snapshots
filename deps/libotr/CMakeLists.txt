cmake_minimum_required( VERSION 2.8.11)
set( LIBOTR otr )
project( ${LIBOTR} )
set( CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules" )

find_package( LibGcrypt REQUIRED )
find_package( LibGpgError REQUIRED )

#Get library version
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h versionFile)
if (NOT versionFile)
	message("Unable to determine libtidy version. ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h file is missing.")
endif()
string(STRIP "${versionFile}" VERSION_TEXT)
string(REGEX REPLACE ".*OTRL_VERSION \"(.*)\".*" "\\1" LIBOTR_VERSION ${VERSION_TEXT})

if(LIBOTR_VERSION)
	message("-- OTR Library version = ${LIBOTR_VERSION}")
else()
	message("*** FAILED to get a VERSION from  ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h!")
endif()

add_definitions(
		-Wall
		${LIBGCRYPT_DEFINITIONS}
		${LIBGPGERROR_DEFINITIONS}
)
include_directories(
		${CMAKE_SOURCE_DIR}
		${CMAKE_CURRENT_BINARY_DIR}
		${LIBGCRYPT_INCLUDE_DIR}
		${LIBGPGERROR_INCLUDE_DIR}
		./src
		./toolkit
		.
)
set( otr_HDRS 
	src/auth.h
	src/b64.h
	src/context.h
	src/context_priv.h
	src/dh.h
	src/instag.h
	src/mem.h
	src/message.h
	src/privkey-t.h
	src/privkey.h
	src/proto.h
	src/serial.h
	src/sm.h
	src/tlv.h
	src/userstate.h
	src/version.h
)

set ( otr_SRCS
	src/auth.c
	src/b64.c
	src/context.c
	src/context_priv.c
	src/dh.c
	src/instag.c
	src/mem.c
	src/message.c
	src/privkey.c
	src/proto.c
	src/sm.c
	src/tlv.c
	src/userstate.c
)

add_library( ${LIBOTR}
	SHARED
	${otr_SRCS}
	${otr_HDRS}
)
if(LIBOTR_VERSION)
set_target_properties( ${LIBOTR} PROPERTIES 
	                VERSION ${LIBOTR_VERSION}
)
endif()
target_link_libraries( ${LIBOTR} ${LIBGCRYPT_LIBRARY} )
INSTALL( FILES ${otr_HDRS} DESTINATION ${CMAKE_INSTALL_PREFIX}/include/libotr )
INSTALL( FILES libotr.m4 DESTINATION ${CMAKE_INSTALL_PREFIX}/share/aclocal )
install(
	TARGETS	${LIBOTR}
	LIBRARY	DESTINATION ${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}
	RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
	ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
)
