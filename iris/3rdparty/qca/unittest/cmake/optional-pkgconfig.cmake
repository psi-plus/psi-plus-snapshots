# Exercise the real provider configuration without a compiler, Qt or pkg-config.
foreach(provider botan gcrypt nss pkcs11)
  foreach(mode auto yes)
    set(directory "${TEST_BINARY_DIR}/${provider}-${mode}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
      -S "${QCA_SOURCE_DIR}/unittest/cmake/pkgconfig-fixture" -B "${directory}"
      -G "${TEST_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}"
      "-DQCA_SOURCE_DIR=${QCA_SOURCE_DIR}" "-DPROVIDER=${provider}" "-DMODE=${mode}"
      -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(mode STREQUAL "auto")
      if(NOT result EQUAL 0)
        message(FATAL_ERROR "Optional ${provider} requires pkg-config:\n${output}\n${error}")
      endif()
    elseif(result EQUAL 0 OR NOT "${output}\n${error}" MATCHES "PkgConfig|pkcs11-helper")
      message(FATAL_ERROR "Explicit ${provider} did not report its missing dependency:\n${output}\n${error}")
    endif()
  endforeach()
endforeach()
