include_guard(GLOBAL)

if(NOT ANDROID)
    return()
endif()

set(IRIS_ANDROID_OPENSSL_ROOT "" CACHE PATH
    "Android OpenSSL bundle root, for example <Android SDK>/android_openssl/ssl_3")

if(NOT IRIS_ANDROID_OPENSSL_ROOT)
    set(_iris_android_openssl_candidates
        "${ANDROID_SDK_ROOT}/android_openssl/ssl_3"
        "$ENV{ANDROID_SDK_ROOT}/android_openssl/ssl_3"
        "$ENV{ANDROID_HOME}/android_openssl/ssl_3"
        "$ENV{HOME}/Android/Sdk/android_openssl/ssl_3")
    foreach(_candidate IN LISTS _iris_android_openssl_candidates)
        if(EXISTS "${_candidate}/include/openssl/opensslv.h")
            set(IRIS_ANDROID_OPENSSL_ROOT "${_candidate}" CACHE PATH
                "Android OpenSSL bundle root, for example <Android SDK>/android_openssl/ssl_3" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT IRIS_ANDROID_OPENSSL_ROOT)
    message(FATAL_ERROR
        "Android OpenSSL was not found. Install the Qt/KDAB android_openssl bundle under "
        "<Android SDK>/android_openssl/ssl_3 or set IRIS_ANDROID_OPENSSL_ROOT explicitly.")
endif()

set(_iris_android_openssl_abi "${CMAKE_ANDROID_ARCH_ABI}")
if(NOT _iris_android_openssl_abi)
    set(_iris_android_openssl_abi "${ANDROID_ABI}")
endif()
if(NOT _iris_android_openssl_abi)
    message(FATAL_ERROR "Neither CMAKE_ANDROID_ARCH_ABI nor ANDROID_ABI is set")
endif()

set(_iris_android_openssl_include "${IRIS_ANDROID_OPENSSL_ROOT}/include")
set(_iris_android_openssl_lib_dir "${IRIS_ANDROID_OPENSSL_ROOT}/${_iris_android_openssl_abi}")
set(_iris_android_openssl_ssl "${_iris_android_openssl_lib_dir}/libssl.a")
set(_iris_android_openssl_crypto "${_iris_android_openssl_lib_dir}/libcrypto.a")
set(_iris_android_openssl_ssl_runtime "${_iris_android_openssl_lib_dir}/libssl_3.so")
set(_iris_android_openssl_crypto_runtime "${_iris_android_openssl_lib_dir}/libcrypto_3.so")

if(NOT EXISTS "${_iris_android_openssl_include}/openssl/opensslv.h")
    message(FATAL_ERROR "Android OpenSSL headers were not found under ${_iris_android_openssl_include}")
endif()
if(NOT EXISTS "${_iris_android_openssl_ssl}" OR NOT EXISTS "${_iris_android_openssl_crypto}")
    message(FATAL_ERROR "Android OpenSSL bundle has no static libraries for ${_iris_android_openssl_abi}")
endif()
if(NOT EXISTS "${_iris_android_openssl_ssl_runtime}" OR NOT EXISTS "${_iris_android_openssl_crypto_runtime}")
    message(FATAL_ERROR "Android OpenSSL bundle has no libssl_3.so/libcrypto_3.so for ${_iris_android_openssl_abi}")
endif()

# Do not run FindOpenSSL while cross-compiling Android: host pkg-config may leak
# host-only dependencies into the target link command.  Create canonical targets
# from the already selected per-ABI files instead.
set(OPENSSL_ROOT_DIR "${IRIS_ANDROID_OPENSSL_ROOT}" CACHE PATH "OpenSSL root directory" FORCE)
set(OPENSSL_INCLUDE_DIR "${_iris_android_openssl_include}" CACHE PATH "OpenSSL include directory" FORCE)
set(OPENSSL_SSL_LIBRARY "${_iris_android_openssl_ssl}" CACHE FILEPATH "OpenSSL SSL library" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${_iris_android_openssl_crypto}" CACHE FILEPATH "OpenSSL Crypto library" FORCE)
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "Prefer static OpenSSL libraries" FORCE)
set(OPENSSL_CRYPTO_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}")
set(OPENSSL_SSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}")
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARIES}")
set(OpenSSL_FOUND TRUE)
set(OPENSSL_FOUND TRUE)

if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
endif()
if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
endif()

set(IRIS_ANDROID_OPENSSL_RUNTIME_LIBRARIES
    "${_iris_android_openssl_crypto_runtime};${_iris_android_openssl_ssl_runtime}"
    CACHE INTERNAL "Android OpenSSL runtime libraries required by Qt Network/QCA")

function(iris_deploy_android_openssl)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET "${_target}")
            message(FATAL_ERROR "iris_deploy_android_openssl(): target '${_target}' does not exist")
        endif()
        get_target_property(_extra "${_target}" QT_ANDROID_EXTRA_LIBS)
        if(NOT _extra OR _extra MATCHES "-NOTFOUND$")
            set(_extra)
        endif()
        list(APPEND _extra ${IRIS_ANDROID_OPENSSL_RUNTIME_LIBRARIES})
        list(REMOVE_DUPLICATES _extra)
        set_property(TARGET "${_target}" PROPERTY QT_ANDROID_EXTRA_LIBS "${_extra}")
    endforeach()
endfunction()

message(STATUS "Iris: using Android OpenSSL from ${IRIS_ANDROID_OPENSSL_ROOT} for ${_iris_android_openssl_abi}")
