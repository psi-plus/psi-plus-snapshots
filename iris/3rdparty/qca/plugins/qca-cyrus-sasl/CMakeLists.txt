if(WITH_cyrus-sasl_PLUGIN STREQUAL "yes")
  find_package(Sasl2 REQUIRED)
else()
  find_package(Sasl2)
endif()

if(SASL2_FOUND)
  enable_plugin("cyrus-sasl")

  include(CheckSymbolExists)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${SASL2_INCLUDE_DIR})
  check_symbol_exists(SASL_CHANNEL_BINDING "sasl/sasl.h" HAVE_SASL_CHANNEL_BINDING)
  check_symbol_exists(SASL_BADBINDING "sasl/sasl.h" HAVE_SASL_BADBINDING)
  check_symbol_exists(SASL_CB_CRYPTO "sasl/sasl.h" HAVE_SASL_CRYPTO_CALLBACKS)
  cmake_pop_check_state()

  set(QCA_SASL_SOURCES qca-cyrus-sasl.cpp)
  add_library(qca-cyrus-sasl ${PLUGIN_TYPE} ${QCA_SASL_SOURCES})

  if(APPLE AND ${PLUGIN_TYPE} STREQUAL "MODULE")
    set_property(TARGET qca-cyrus-sasl PROPERTY SUFFIX ".dylib")
  endif()

  target_link_libraries(qca-cyrus-sasl Qt${QT_MAJOR_VERSION}::Core ${QCA_LIB_NAME} Sasl2::Sasl2)

  if(HAVE_SASL_CRYPTO_CALLBACKS)
    target_sources(qca-cyrus-sasl PRIVATE saslcrypto.h)
    target_compile_definitions(qca-cyrus-sasl PRIVATE HAVE_SASL_CRYPTO_CALLBACKS)
  endif()

  # A static library may still contain the built-in OpenSSL backend even
  # when its headers expose crypto callbacks. Probe the actual dependency.
  if(SASL2_STATIC)
    if(WIN32)
      # Cyrus' public Windows headers otherwise declare the API dllimport.
      # The private dependency bundle is a real static library.
      target_compile_definitions(qca-cyrus-sasl PRIVATE WIN32 LIBSASL_EXPORTS)
    endif()
    include(CheckCSourceCompiles)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES ${SASL2_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES Sasl2::Sasl2)
    if(WIN32)
      set(CMAKE_REQUIRED_DEFINITIONS -DWIN32 -DLIBSASL_EXPORTS)
    endif()
    unset(SASL2_LINKS_WITHOUT_CRYPTO CACHE)
    check_c_source_compiles("#include <sasl/sasl.h>
      int main(void) {
        return sasl_client_init(0) | sasl_server_init(0, \"qca\");
      }" SASL2_LINKS_WITHOUT_CRYPTO)
    cmake_pop_check_state()
    if(NOT SASL2_LINKS_WITHOUT_CRYPTO)
      include(QcaOpenSSL)
      qca_find_openssl(REQUIRED)
      target_link_libraries(qca-cyrus-sasl OpenSSL::Crypto)
    endif()
  endif()
  if(HAVE_SASL_CHANNEL_BINDING AND HAVE_SASL_BADBINDING)
    target_compile_definitions(qca-cyrus-sasl PRIVATE HAVE_SASL_CHANNEL_BINDING)
  endif()

  if(NOT DEVELOPER_MODE)
    install(TARGETS qca-cyrus-sasl
            LIBRARY DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            ARCHIVE DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            RUNTIME DESTINATION "${QCA_CRYPTO_INSTALL_DIR}")

    install_pdb(qca-cyrus-sasl ${QCA_CRYPTO_INSTALL_DIR})
  endif()
else()
  disable_plugin("cyrus-sasl")
endif()
