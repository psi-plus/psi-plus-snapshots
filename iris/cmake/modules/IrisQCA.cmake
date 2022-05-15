cmake_minimum_required(VERSION 3.10.0)

set(IrisQCAGitRepo "https://github.com/psi-im/qca.git")

if(BUNDLED_QCA)
    message(STATUS "QCA: using bundled")
    set(QCA_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/qca)
    set(QCA_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/qca)
    set(QCA_BUILD_DIR ${QCA_PREFIX}/build)
    set(Qca_INCLUDE_DIR ${QCA_BUILD_DIR})
    if(NOT EXISTS ${QCA_SOURCE_DIR})
        list(APPEND Qca_INCLUDE_DIR ${QCA_PREFIX}/src/QcaProject/include/QtCrypto)
    else()
        list(APPEND Qca_INCLUDE_DIR ${QCA_SOURCE_DIR}/include/QtCrypto)
    endif()
    set(Qca_CORE_LIB ${QCA_BUILD_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}qca-qt5${D}${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(Qca_OSSL_LIB ${QCA_BUILD_DIR}/lib/qca-qt5/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl${D}${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(Qca_LIBRARY ${Qca_OSSL_LIB} ${Qca_CORE_LIB})
    if(APPLE)
        set(COREFOUNDATION_LIBRARY "-framework CoreFoundation")
        set(COREFOUNDATION_LIBRARY_SECURITY "-framework Security")
        set(Qca_LIBRARY ${Qca_LIBRARY} ${COREFOUNDATION_LIBRARY} ${COREFOUNDATION_LIBRARY_SECURITY})
    endif()
    if(IS_SUBPROJECT)
        set(Qca_LIBRARY_EXPORT ${Qca_LIBRARY} PARENT_SCOPE)
        set(Qca_INCLUDE_DIR_EXPORT ${Qca_INCLUDE_DIR} PARENT_SCOPE)
    endif()

    if ("${OPENSSL_ROOT_DIR}" STREQUAL "" AND APPLE)
        set(OPENSSL_ROOT_DIR /usr/local/opt/openssl)
    endif()
    find_package(OpenSSL REQUIRED)

    include(ExternalProject)
    #set CMake options and transfer the environment to an external project
    set(QCA_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF -DBUILD_PLUGINS=ossl -DLOAD_SHARED_PLUGINS=OFF
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${QCA_PREFIX} -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DOSX_FRAMEWORK=OFF)
    if (EXISTS ${QCA_SOURCE_DIR})
        message(STATUS "QCA: found bundled sources")
        ExternalProject_Add(QcaProject
            PREFIX ${QCA_PREFIX}
            BINARY_DIR ${QCA_BUILD_DIR}
            SOURCE_DIR ${QCA_SOURCE_DIR}
            CMAKE_ARGS ${QCA_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${Qca_LIBRARY}
            INSTALL_COMMAND ""
            )
    else()
        include(FindGit)
        find_package(Git)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "Git not found! Bundled Qca needs Git utility.\nPlease set GIT_EXECUTABLE variable or add git to PATH")
        endif()
        ExternalProject_Add(QcaProject
            PREFIX ${QCA_PREFIX}
            BINARY_DIR ${QCA_BUILD_DIR}
            GIT_REPOSITORY ${IrisQCAGitRepo}
            CMAKE_ARGS ${QCA_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${Qca_LIBRARY}
            INSTALL_COMMAND ""
            )
    endif()
else()
    message(WARNING "Disabling BUNDLED_QCA option makes impossible to use DTLS and PsiMedia")
    message(STATUS "QCA: using system")
    find_package(Qca REQUIRED)
endif()

set(QCA_INCLUDES ${Qca_INCLUDE_DIR})
set(qca_LIB ${Qca_LIBRARY})

include_directories(
    ${Qca_INCLUDE_DIR}
)
