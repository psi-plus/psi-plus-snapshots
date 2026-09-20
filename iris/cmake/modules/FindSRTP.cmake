# libSRTP 2 public API. Optional dependency for the native RTP packet path.
find_path(SRTP_INCLUDE_DIR NAMES srtp2/srtp.h)
find_library(SRTP_LIBRARY NAMES srtp2)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SRTP REQUIRED_VARS SRTP_INCLUDE_DIR SRTP_LIBRARY)
if(SRTP_FOUND AND NOT TARGET SRTP::SRTP)
    add_library(SRTP::SRTP UNKNOWN IMPORTED)
    set_target_properties(SRTP::SRTP PROPERTIES
        IMPORTED_LOCATION "${SRTP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SRTP_INCLUDE_DIR}")
endif()
mark_as_advanced(SRTP_INCLUDE_DIR SRTP_LIBRARY)
