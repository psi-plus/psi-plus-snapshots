#=============================================================================
# Copyright (C) 2016-2020  Psi+ Project, Vitaly Tonkacheyev
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 3. The name of the author may not be used to endorse or promote products
#    derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
# NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#=============================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32)
    set(D "d")
endif()
if( B2_INCLUDE_DIR AND B2_LIBRARY )
    # in cache already
    set(B2_FIND_QUIETLY TRUE)
endif( B2_INCLUDE_DIR AND B2_LIBRARY )

if( UNIX AND NOT( APPLE OR CYGWIN ) )
    find_package( PkgConfig QUIET )
    pkg_check_modules( PC_B2 QUIET libb2 )
    if( PC_B2_FOUND )
        set( B2_DEFINITIONS ${PC_B2_CFLAGS} ${PC_B2_CFLAGS_OTHER} )
    endif( PC_B2_FOUND )
endif( UNIX AND NOT( APPLE OR CYGWIN ) )

set( B2_ROOT "" CACHE STRING "Path to libb2 library" )

find_path(
    B2_INCLUDE_DIR blake2.h
    HINTS
    ${B2_ROOT}/include
    ${PC_B2_INCLUDEDIR}
    ${PC_B2_INCLUDE_DIRS}
)
set(B2_NAMES
    b2${D}
    libb2${D}
)
find_library(
    B2_LIBRARY
    NAMES ${B2_NAMES}
    HINTS
    ${PC_B2_LIBDIR}
    ${PC_B2_LIBRARY_DIRS}
    ${B2_ROOT}/lib
    ${B2_ROOT}/bin
)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
                B2
                DEFAULT_MSG
                B2_LIBRARY
                B2_INCLUDE_DIR
)
if( B2_FOUND )
    set( B2_LIBRARIES ${B2_LIBRARY} )
    set( B2_INCLUDE_DIRS ${B2_INCLUDE_DIR} )
endif( B2_FOUND )

mark_as_advanced( B2_INCLUDE_DIR B2_LIBRARY )
