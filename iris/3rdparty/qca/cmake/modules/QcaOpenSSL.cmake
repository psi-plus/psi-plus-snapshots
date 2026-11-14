# SPDX-License-Identifier: BSD-3-Clause
# Locate OpenSSL for QCA, including Qt's Android OpenSSL convenience package.

include_guard(GLOBAL)

include(CMakeParseArguments)

if(ANDROID)
  set(QCA_ANDROID_OPENSSL_ROOT "" CACHE PATH
      "Path to a KDAB android_openssl checkout")
  set(QCA_ANDROID_OPENSSL_URL
      "https://github.com/KDAB/android_openssl/archive/refs/heads/master.zip"
      CACHE STRING "URL used to fetch android_openssl when no local checkout is found")
  option(QCA_ANDROID_OPENSSL_FETCH
         "Fetch android_openssl when building for Android and no local checkout is found"
         ON)
endif()

function(_qca_android_openssl_checkout_from_path input_path output_variable)
  set(_qca_candidate "${input_path}")
  set(_qca_checkout "")

  # OPENSSL_ROOT_DIR may point to the checkout itself, ssl_3/ssl_1.1,
  # no-asm/ssl_3, or even an ABI directory. Walk a few levels upward and
  # look for the KDAB integration module instead of treating this layout as a
  # regular FindOpenSSL prefix.
  foreach(_qca_level RANGE 0 3)
    if(EXISTS "${_qca_candidate}/android_openssl.cmake")
      set(_qca_checkout "${_qca_candidate}")
      break()
    endif()

    get_filename_component(_qca_parent "${_qca_candidate}" DIRECTORY)
    if(_qca_parent STREQUAL _qca_candidate)
      break()
    endif()
    set(_qca_candidate "${_qca_parent}")
  endforeach()

  set(${output_variable} "${_qca_checkout}" PARENT_SCOPE)
endfunction()

function(_qca_include_android_openssl checkout_root)
  message(STATUS "QCA: using Android OpenSSL from ${checkout_root}")
  include("${checkout_root}/android_openssl.cmake")
  set(QCA_ANDROID_OPENSSL_ROOT
      "${checkout_root}"
      CACHE PATH "Path to a KDAB android_openssl checkout" FORCE)
endfunction()

function(qca_find_openssl)
  cmake_parse_arguments(QCA_OPENSSL "REQUIRED" "" "" ${ARGN})

  if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    return()
  endif()

  if(TARGET OpenSSL::SSL OR TARGET OpenSSL::Crypto)
    message(FATAL_ERROR
      "Only one of OpenSSL::SSL and OpenSSL::Crypto exists. "
      "QCA requires both imported targets.")
  endif()

  if(NOT ANDROID)
    if(QCA_OPENSSL_REQUIRED)
      find_package(OpenSSL 1.1.1 REQUIRED)
    else()
      find_package(OpenSSL 1.1.1 QUIET)
    endif()
    return()
  endif()

  # OPENSSL_ROOT_DIR can also point at KDAB's non-standard Android layout,
  # for example android_openssl/ssl_3. In that layout headers live in
  # ssl_3/include while libraries live in ssl_3/<ABI>, so FindOpenSSL cannot
  # treat it as a conventional installation prefix. Detect it before trying
  # the standard package finder.
  if(OPENSSL_ROOT_DIR)
    _qca_android_openssl_checkout_from_path(
      "${OPENSSL_ROOT_DIR}" _qca_checkout_from_openssl_root)

    if(_qca_checkout_from_openssl_root)
      _qca_include_android_openssl(
        "${_qca_checkout_from_openssl_root}")

      if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        return()
      endif()
    else()
      # This looks like a conventional OpenSSL prefix. Disable host
      # pkg-config discovery for the Android cross-build so that a host
      # openssl.pc cannot contaminate the result.
      if(DEFINED CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)
        set(_qca_saved_disable_pkgconfig
            "${CMAKE_DISABLE_FIND_PACKAGE_PkgConfig}")
        set(_qca_had_disable_pkgconfig TRUE)
      else()
        set(_qca_had_disable_pkgconfig FALSE)
      endif()

      set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
      find_package(OpenSSL 1.1.1 QUIET)

      if(_qca_had_disable_pkgconfig)
        set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig
            "${_qca_saved_disable_pkgconfig}")
      else()
        unset(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)
      endif()

      if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        message(STATUS
          "QCA: using conventional OpenSSL installation from OPENSSL_ROOT_DIR")
        return()
      endif()
    endif()
  endif()

  set(_qca_android_openssl_candidates)

  if(QCA_ANDROID_OPENSSL_ROOT)
    list(APPEND _qca_android_openssl_candidates
         "${QCA_ANDROID_OPENSSL_ROOT}")
  endif()

  foreach(_qca_sdk_variable
          QT_ANDROID_SDK_ROOT
          ANDROID_SDK_ROOT
          ANDROID_HOME)
    if(DEFINED ${_qca_sdk_variable}
       AND NOT "${${_qca_sdk_variable}}" STREQUAL "")
      list(APPEND _qca_android_openssl_candidates
           "${${_qca_sdk_variable}}/android_openssl")
    endif()
  endforeach()

  foreach(_qca_sdk_environment ANDROID_SDK_ROOT ANDROID_HOME)
    if(DEFINED ENV{${_qca_sdk_environment}}
       AND NOT "$ENV{${_qca_sdk_environment}}" STREQUAL "")
      file(TO_CMAKE_PATH "$ENV{${_qca_sdk_environment}}"
           _qca_sdk_environment_path)
      list(APPEND _qca_android_openssl_candidates
           "${_qca_sdk_environment_path}/android_openssl")
    endif()
  endforeach()

  if(CMAKE_ANDROID_NDK)
    get_filename_component(_qca_android_ndk_parent
                           "${CMAKE_ANDROID_NDK}" DIRECTORY)
    get_filename_component(_qca_android_sdk_from_ndk
                           "${_qca_android_ndk_parent}" DIRECTORY)
    list(APPEND _qca_android_openssl_candidates
         "${_qca_android_sdk_from_ndk}/android_openssl")
  endif()

  if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{HOME}" _qca_home_path)
    list(APPEND _qca_android_openssl_candidates
         "${_qca_home_path}/Android/Sdk/android_openssl")
  endif()

  list(REMOVE_DUPLICATES _qca_android_openssl_candidates)

  foreach(_qca_android_openssl_path
          IN LISTS _qca_android_openssl_candidates)
    _qca_android_openssl_checkout_from_path(
      "${_qca_android_openssl_path}" _qca_android_openssl_root)
    if(_qca_android_openssl_root)
      _qca_include_android_openssl("${_qca_android_openssl_root}")
      break()
    endif()
  endforeach()

  if(NOT TARGET OpenSSL::SSL
     AND NOT TARGET OpenSSL::Crypto
     AND QCA_ANDROID_OPENSSL_FETCH)
    include(FetchContent)
    FetchContent_Declare(
      android_openssl
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      URL "${QCA_ANDROID_OPENSSL_URL}"
    )
    FetchContent_MakeAvailable(android_openssl)
    include("${android_openssl_SOURCE_DIR}/android_openssl.cmake")
    message(STATUS
      "QCA: using fetched Android OpenSSL from ${android_openssl_SOURCE_DIR}")
  endif()

  if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    return()
  endif()

  if(TARGET OpenSSL::SSL OR TARGET OpenSSL::Crypto)
    message(FATAL_ERROR
      "Android OpenSSL setup created only one of OpenSSL::SSL and "
      "OpenSSL::Crypto.")
  endif()

  if(QCA_OPENSSL_REQUIRED)
    message(FATAL_ERROR
      "OpenSSL 1.1.1 or newer is required for qca-ossl. Provide existing "
      "OpenSSL::SSL and OpenSSL::Crypto targets, set OPENSSL_ROOT_DIR, set "
      "QCA_ANDROID_OPENSSL_ROOT to a KDAB android_openssl checkout, or enable "
      "QCA_ANDROID_OPENSSL_FETCH.")
  endif()
endfunction()
