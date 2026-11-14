# QCA WinCrypto: Windows CNG (bcrypt.dll)

if(WIN32)
  enable_plugin("wincrypto")

  set(QCA_WINCRYPTO_SOURCES qca-wincrypto.cpp aesmodes.cpp aesmodes.h schannel.cpp native_tls.h native_keys.cpp native_keys.h rsa_codec.cpp rsa_codec.h)
  add_library(qca-wincrypto ${PLUGIN_TYPE} ${QCA_WINCRYPTO_SOURCES})

  target_compile_definitions(qca-wincrypto PRIVATE NOMINMAX)
  target_link_libraries(qca-wincrypto Qt${QT_MAJOR_VERSION}::Core)
  target_link_libraries(qca-wincrypto ${QCA_LIB_NAME})
  target_link_libraries(qca-wincrypto bcrypt crypt32 secur32 ncrypt)

  if(NOT DEVELOPER_MODE)
    install(TARGETS qca-wincrypto
            LIBRARY DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            ARCHIVE DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            RUNTIME DESTINATION "${QCA_CRYPTO_INSTALL_DIR}")

    install_pdb(qca-wincrypto ${QCA_CRYPTO_INSTALL_DIR})
  endif()
else()
  disable_plugin("wincrypto")
endif()
