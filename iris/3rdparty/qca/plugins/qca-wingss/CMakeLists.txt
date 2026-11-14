if(WIN32)
  enable_plugin("wingss")

  add_library(qca-wingss ${PLUGIN_TYPE} qca-wingss.cpp gssapi_p.h)
  target_compile_definitions(qca-wingss PRIVATE NOMINMAX UNICODE _UNICODE)
  target_link_libraries(qca-wingss Qt${QT_MAJOR_VERSION}::Core ${QCA_LIB_NAME})

  if(NOT DEVELOPER_MODE)
    install(TARGETS qca-wingss
            LIBRARY DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            ARCHIVE DESTINATION "${QCA_CRYPTO_INSTALL_DIR}"
            RUNTIME DESTINATION "${QCA_CRYPTO_INSTALL_DIR}")
    install_pdb(qca-wingss ${QCA_CRYPTO_INSTALL_DIR})
  endif()
else()
  disable_plugin("wingss")
endif()
