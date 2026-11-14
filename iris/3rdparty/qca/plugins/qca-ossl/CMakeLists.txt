# QCA OSSL

include(QcaOpenSSL)

if(WITH_ossl_PLUGIN STREQUAL "yes")
  qca_find_openssl(REQUIRED)
else()
  qca_find_openssl()
endif()

if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
  enable_plugin("ossl")

  include(CheckSymbolExists)
  include(CMakePushCheckState)

  cmake_push_check_state(RESET)
  get_target_property(_qca_openssl_include_dirs
                      OpenSSL::Crypto INTERFACE_INCLUDE_DIRECTORIES)
  if(_qca_openssl_include_dirs)
    set(CMAKE_REQUIRED_INCLUDES "${_qca_openssl_include_dirs}")
  endif()
  set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)

  macro(openssl_evp_detect symbol feature)
    check_symbol_exists(${symbol} "openssl/evp.h" HAVE_OPENSSL_${feature})
    if(HAVE_OPENSSL_${feature})
      list(APPEND QCA_OSSL_COMPILE_DEFINITIONS HAVE_OPENSSL_${feature})
    endif()
  endmacro()

  openssl_evp_detect(EVP_sha SHA0)
  openssl_evp_detect(EVP_md2 MD2)
  openssl_evp_detect(EVP_aes_128_ccm AES_CCM)
  openssl_evp_detect(EVP_aes_128_ctr AES_CTR)
  openssl_evp_detect(EVP_aes_128_gcm AES_GCM)
  openssl_evp_detect(EVP_sha3_224 SHA3_224)
  openssl_evp_detect(EVP_sha3_256 SHA3_256)
  openssl_evp_detect(EVP_sha3_384 SHA3_384)
  openssl_evp_detect(EVP_sha3_512 SHA3_512)
  openssl_evp_detect(EVP_blake2b512 BLAKE2B_512)

  cmake_pop_check_state()

  set(QCA_OSSL_SOURCES
      qca-ossl.cpp
      basetlscontext.cpp
      basetlscontext.h
      dtlscontext.cpp
      dtlscontext.h
      tlscontext.cpp
      tlscontext.h
      certcontext.cpp
      certcontext.h
      pkeycontext.cpp
      pkeycontext.h
      utils.cpp
      utils.h
      keyutils.cpp
      keyutils.h
      rsakey.cpp
      rsakey.h
      ed25519key.cpp
      ed25519key.h
      dsakey.cpp
      dsakey.h
      dhkey.cpp
      dhkey.h
      evpkey.cpp
      evpkey.h
      )

  add_library(qca-ossl ${PLUGIN_TYPE} ${QCA_OSSL_SOURCES})

  if(APPLE AND ${PLUGIN_TYPE} STREQUAL "MODULE")
    set_property(TARGET qca-ossl PROPERTY SUFFIX ".dylib")
  endif()


  target_compile_definitions(qca-ossl PRIVATE ${QCA_OSSL_COMPILE_DEFINITIONS})
  target_link_libraries(qca-ossl Qt${QT_MAJOR_VERSION}::Core)
  target_link_libraries(qca-ossl ${QCA_LIB_NAME})
  target_link_libraries(qca-ossl OpenSSL::SSL OpenSSL::Crypto)
  if(WIN32)
    # FindOpenSSL can construct OpenSSL targets from raw static archives.
    # Unlike package-provided targets, those omit OpenSSL's Windows system
    # libraries; the provider links OpenSSL directly and must carry them.
    target_link_libraries(qca-ossl ws2_32 crypt32 advapi32 user32 bcrypt)
  endif()

  if(NOT DEVELOPER_MODE)
    install(TARGETS qca-ossl
            LIBRARY DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            ARCHIVE DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            RUNTIME DESTINATION "${QCA_CRYPTO_INSTALL_DIR}")

    install_pdb(qca-ossl ${QCA_CRYPTO_INSTALL_DIR})
  endif()

else()
  disable_plugin("ossl")
endif()
