cmake_minimum_required(VERSION 3.11.0)

set(IRIS_BUNDLED_QCA_GIT_REPOSITORY "https://github.com/psi-im/qca.git" CACHE STRING
    "Bundled QCA git repository")
set(IRIS_BUNDLED_QCA_GIT_TAG "master" CACHE STRING "Bundled QCA git tag or branch")
set(IRIS_QCA_SOURCE_DIR "" CACHE PATH "Local QCA source directory")
set(IRIS_SYSTEM_QCA "AUTO" CACHE STRING "System QCA generation: AUTO, 2 or 3")
set_property(CACHE IRIS_SYSTEM_QCA PROPERTY STRINGS AUTO 2 3)

string(TOUPPER "${IRIS_SYSTEM_QCA}" _iris_system_qca)
if(NOT _iris_system_qca MATCHES "^(AUTO|2|3)$")
    message(FATAL_ERROR "IRIS_SYSTEM_QCA must be AUTO, 2 or 3")
endif()

if(IRIS_BUNDLED_QCA)
    include(GNUInstallDirs)
    message(STATUS "QCA: using bundled psi-im/qca (QCA 3)")
    set(IRIS_QCA_MAJOR 3)
    set(IRIS_QCA_PACKAGE "Qca3-qt${QT_DEFAULT_MAJOR_VERSION}")
    set(QCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/qca")
    if(IRIS_QCA_SOURCE_DIR)
        set(QCA_SOURCE_DIR "${IRIS_QCA_SOURCE_DIR}")
    endif()
    set(QCA_PREFIX "${CMAKE_BINARY_DIR}/_deps/qca")
    set(IRIS_QCA_INSTALL_DIR "${QCA_PREFIX}/install")
    set(Qca_INCLUDE_DIR "${IRIS_QCA_INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}/Qca3-qt${QT_DEFAULT_MAJOR_VERSION}/QtCrypto")
    if(NOT EXISTS "${QCA_SOURCE_DIR}")
        set(_qca_source_args
            GIT_REPOSITORY ${IRIS_BUNDLED_QCA_GIT_REPOSITORY}
            GIT_TAG "${IRIS_BUNDLED_QCA_GIT_TAG}"
            GIT_SHALLOW TRUE GIT_PROGRESS TRUE
            )
        set(_qca_source_identity
            "git:${IRIS_BUNDLED_QCA_GIT_REPOSITORY}@${IRIS_BUNDLED_QCA_GIT_TAG}")
    else()
        get_filename_component(_qca_source_realpath "${QCA_SOURCE_DIR}" REALPATH)
        message(STATUS "QCA: found local sources at ${_qca_source_realpath}")
        set(_qca_source_args SOURCE_DIR "${_qca_source_realpath}" DOWNLOAD_COMMAND "")
        set(_qca_source_identity "local:${_qca_source_realpath}")
    endif()

    # ExternalProject's default binary directory is derived only from the
    # project name. Reconfiguring Iris after switching between the downloaded
    # QCA tree and IRIS_QCA_SOURCE_DIR would otherwise reuse a CMakeCache.txt
    # generated for a different source directory. Give each source identity
    # its own binary directory instead.
    string(SHA256 _qca_source_hash "${_qca_source_identity}")
    string(SUBSTRING "${_qca_source_hash}" 0 12 _qca_source_hash_short)
    set(_qca_binary_dir "${QCA_PREFIX}/build-${_qca_source_hash_short}")

    set(Qca_CORE_LIB "${IRIS_QCA_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}qca3-qt${QT_DEFAULT_MAJOR_VERSION}${D}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(Qca_OSSL_LIB "${IRIS_QCA_INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/qca3-qt${QT_DEFAULT_MAJOR_VERSION}/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl${D}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(Qca_LIBRARY ${Qca_OSSL_LIB} ${Qca_CORE_LIB})
    if(APPLE)
        set(Qca_LIBRARY ${Qca_LIBRARY} "-framework CoreFoundation" "-framework Security")
    endif()
    if(IS_SUBPROJECT)
        set(Qca_LIBRARY_EXPORT ${Qca_LIBRARY} PARENT_SCOPE)
        set(Qca_INCLUDE_DIR_EXPORT ${Qca_INCLUDE_DIR} PARENT_SCOPE)
    endif()

    if(ANDROID)
        include(IrisAndroidOpenSSL)
    else()
        if("${OPENSSL_ROOT_DIR}" STREQUAL "" AND APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(OPENSSL_ROOT_DIR /usr/local/opt/openssl)
        endif()
        find_package(OpenSSL REQUIRED)
    endif()

    if(CMAKE_CONFIGURATION_TYPES)
        set(_qca_build_config "$<CONFIG>")
    else()
        set(_qca_build_config "${CMAKE_BUILD_TYPE}")
    endif()

    include(ExternalProject)
    string(REPLACE ";" "|" _iris_qca_osx_architectures "${CMAKE_OSX_ARCHITECTURES}")
    string(REPLACE ";" "|" _iris_qca_find_root_path "${CMAKE_FIND_ROOT_PATH}")
    set(QCA_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_PLUGINS=ossl
        -DLOAD_SHARED_PLUGINS=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_TOOLS=OFF
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        "-DLIB_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_LIBDIR}"
        "-DINCLUDE_INSTALL_DIR=<INSTALL_DIR>/${CMAKE_INSTALL_INCLUDEDIR}"
        -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
        -DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}
        -DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}
        -DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}
        -DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_SYSROOT=${CMAKE_SYSROOT}
        -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
        -DCMAKE_OSX_ARCHITECTURES=${_iris_qca_osx_architectures}
        -DCMAKE_FIND_ROOT_PATH=${_iris_qca_find_root_path}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DANDROID_NDK=${ANDROID_NDK}
        -DCMAKE_ANDROID_NDK=${CMAKE_ANDROID_NDK}
        -DCMAKE_ANDROID_ARCH_ABI=${CMAKE_ANDROID_ARCH_ABI}
        -DCMAKE_ANDROID_API=${CMAKE_ANDROID_API}
        -DQT_HOST_PATH=${QT_HOST_PATH}
        -DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}
        -DQt6_DIR=${Qt6_DIR}
        -DQt6Core_DIR=${Qt6Core_DIR}
        -DOSX_FRAMEWORK=OFF)
    if(QT_DEFAULT_MAJOR_VERSION LESS 6)
        list(APPEND QCA_BUILD_OPTIONS -DBUILD_WITH_QT6=OFF)
    else()
        list(APPEND QCA_BUILD_OPTIONS -DBUILD_WITH_QT6=ON)
    endif()
    list(APPEND QCA_BUILD_OPTIONS -DQCA_SUFFIX=qt${QT_DEFAULT_MAJOR_VERSION})
    file(MAKE_DIRECTORY "${Qca_INCLUDE_DIR}")
    ExternalProject_Add(QcaProject
        ${_qca_source_args}
        UPDATE_COMMAND ""
        PREFIX ${QCA_PREFIX}
        BINARY_DIR "${_qca_binary_dir}"
        INSTALL_DIR "${IRIS_QCA_INSTALL_DIR}"
        LIST_SEPARATOR "|"
        CMAKE_ARGS ${QCA_BUILD_OPTIONS}
        BUILD_BYPRODUCTS ${Qca_CORE_LIB} ${Qca_OSSL_LIB}
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config "${_qca_build_config}"
        )
    add_library(qca-core STATIC IMPORTED GLOBAL)
    set_target_properties(qca-core PROPERTIES
        IMPORTED_LOCATION "${Qca_CORE_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${Qca_INCLUDE_DIR}"
    )
    add_library(qca-ossl STATIC IMPORTED GLOBAL)
    set_target_properties(qca-ossl PROPERTIES
        IMPORTED_LOCATION "${Qca_OSSL_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${Qca_INCLUDE_DIR}"
    )
    add_library(Qca3::Qca INTERFACE IMPORTED GLOBAL)
    set_property(TARGET Qca3::Qca PROPERTY INTERFACE_LINK_LIBRARIES
        "qca-core;qca-ossl;OpenSSL::Crypto;OpenSSL::SSL"
    )
    if(WIN32 OR USE_MXE)
        set_property(TARGET Qca3::Qca APPEND PROPERTY INTERFACE_LINK_LIBRARIES crypt32 ws2_32)
    endif()
    set_property(TARGET Qca3::Qca PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${Qca_INCLUDE_DIR}")
    set(IRIS_QCA_TARGET Qca3::Qca)
    add_dependencies(qca-core QcaProject)
    add_dependencies(qca-ossl QcaProject)
else()
    message(STATUS "QCA: using system QCA (${_iris_system_qca})")
    set(_iris_qca3_package "Qca3-qt${QT_DEFAULT_MAJOR_VERSION}")

    if(_iris_system_qca STREQUAL "AUTO" OR _iris_system_qca STREQUAL "3")
        if(TARGET Qca3::Qca)
            set(IRIS_QCA_TARGET Qca3::Qca)
        else()
            find_package(${_iris_qca3_package} CONFIG QUIET)
            if(TARGET Qca3::Qca)
                set(IRIS_QCA_TARGET Qca3::Qca)
            endif()
        endif()
        if(IRIS_QCA_TARGET)
            set(IRIS_QCA_MAJOR 3)
            set(IRIS_QCA_PACKAGE "${_iris_qca3_package}")
        elseif(_iris_system_qca STREQUAL "3")
            message(FATAL_ERROR "System QCA 3 requested but ${_iris_qca3_package} was not found")
        endif()
    endif()

    if(NOT IRIS_QCA_TARGET)
        if(TARGET Qca::Qca)
            set(Qca_FOUND TRUE)
        else()
            find_package(Qca QUIET)
        endif()
        if(TARGET Qca::Qca)
            set(IRIS_QCA_TARGET Qca::Qca)
            set(IRIS_QCA_MAJOR 2)
            set(IRIS_QCA_PACKAGE "Qca")
        else()
            message(FATAL_ERROR
                "No usable system QCA found. Install QCA 3 (preferred) or QCA 2, "
                "or enable IRIS_BUNDLED_QCA")
        endif()
    endif()

    message(STATUS "QCA: selected system QCA ${IRIS_QCA_MAJOR} (${IRIS_QCA_TARGET})")
endif()
