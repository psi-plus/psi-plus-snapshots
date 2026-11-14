# - Find Cyrus SASL 2
#
# Variables:
#   SASL2_FOUND
#   SASL2_INCLUDE_DIR
#   SASL2_LIBRARIES
#   SASL2_STATIC
#
# Hints:
#   Sasl2_ROOT            standard CMake package root
#   SASL2_ROOT            legacy QCA alias for Sasl2_ROOT
#   SASL2_USE_STATIC_LIBS prefer the private static dependency bundle
#
# Imported target:
#   Sasl2::Sasl2

include(FindPackageHandleStandardArgs)

set(SASL2_ROOT "" CACHE PATH "Legacy Cyrus SASL installation prefix")
if(Sasl2_ROOT)
  set(_sasl2_root "${Sasl2_ROOT}")
else()
  set(_sasl2_root "${SASL2_ROOT}")
endif()
option(SASL2_USE_STATIC_LIBS "Prefer a static Cyrus SASL library" OFF)

set(_sasl2_path_options)
if(_sasl2_root)
  # An explicit package root is already a target installation prefix.  In a
  # cross build (notably Android), CMake would otherwise prepend the NDK
  # sysroot to this path and make a private dependency bundle undiscoverable.
  set(_sasl2_path_options NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()

find_path(SASL2_INCLUDE_DIR
  NAMES sasl/sasl.h
  HINTS "${_sasl2_root}"
  PATH_SUFFIXES include
  ${_sasl2_path_options})

if(SASL2_USE_STATIC_LIBS)
  if(WIN32)
    set(_sasl2_names sasl2-static libsasl2-static sasl2)
  else()
    set(_sasl2_names sasl2)
  endif()
else()
  set(_sasl2_names sasl2 libsasl)
endif()

find_library(SASL2_LIBRARIES
  NAMES ${_sasl2_names}
  HINTS "${_sasl2_root}"
  PATH_SUFFIXES lib lib64
  ${_sasl2_path_options})

find_package_handle_standard_args(Sasl2
  REQUIRED_VARS SASL2_INCLUDE_DIR SASL2_LIBRARIES)
# Preserve the historical variable used by QCA and existing consumers.
set(SASL2_FOUND "${Sasl2_FOUND}")

set(SASL2_STATIC OFF)
if(SASL2_FOUND)
  get_filename_component(_sasl2_library_name "${SASL2_LIBRARIES}" NAME)
  if(SASL2_USE_STATIC_LIBS OR
     _sasl2_library_name MATCHES "(^|-)static\\.lib$" OR
     _sasl2_library_name MATCHES "\\.a$")
    set(SASL2_STATIC ON)
  endif()

  if(NOT TARGET Sasl2::Sasl2)
    add_library(Sasl2::Sasl2 UNKNOWN IMPORTED)
    set_target_properties(Sasl2::Sasl2 PROPERTIES
      IMPORTED_LOCATION "${SASL2_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${SASL2_INCLUDE_DIR}")
  endif()

  if(SASL2_STATIC)
    if(WIN32)
      set_property(TARGET Sasl2::Sasl2 APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "ws2_32;advapi32;crypt32;bcrypt")
    elseif(UNIX)
      find_package(Threads REQUIRED)
      set_property(TARGET Sasl2::Sasl2 APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES Threads::Threads)
      if(CMAKE_DL_LIBS)
        set_property(TARGET Sasl2::Sasl2 APPEND PROPERTY
          INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}")
      endif()
    endif()
  endif()
endif()

mark_as_advanced(SASL2_INCLUDE_DIR SASL2_LIBRARIES)
