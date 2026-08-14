cmake_minimum_required(VERSION 3.10.0)

set(IRIS_BUNDLED_QCA_GIT_REPOSITORY "https://github.com/psi-im/qca.git" CACHE STRING
    "Bundled QCA git repository")
set(IRIS_BUNDLED_QCA_GIT_TAG "master" CACHE STRING "Bundled QCA git tag or branch")
set(IRIS_QCA_SOURCE_DIR "" CACHE PATH "Local QCA source directory")

if(IRIS_BUNDLED_QCA)
    message(STATUS "QCA: using bundled psi-im/qca")
    set(QCA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/qca")
    if(IRIS_QCA_SOURCE_DIR)
        set(QCA_SOURCE_DIR "${IRIS_QCA_SOURCE_DIR}")
    endif()
    set(QCA_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/qca")
    set(QCA_BUILD_DIR "${QCA_PREFIX}/build")
    set(Qca_INCLUDE_DIR "${QCA_BUILD_DIR}")
    if(NOT EXISTS "${QCA_SOURCE_DIR}")
        list(APPEND Qca_INCLUDE_DIR "${QCA_PREFIX}/src/QcaProject/include/QtCrypto")
    else()
        list(APPEND Qca_INCLUDE_DIR "${QCA_SOURCE_DIR}/include/QtCrypto")
    endif()

    set(Qca_CORE_LIB "${QCA_BUILD_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}qca-qt${QT_DEFAULT_MAJOR_VERSION}${D}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(Qca_OSSL_LIB "${QCA_BUILD_DIR}/lib/qca-qt${QT_DEFAULT_MAJOR_VERSION}/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl${D}${CMAKE_STATIC_LIBRARY_SUFFIX}")
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
        # The historical macOS convenience path is invalid for an iOS cross-build.
        if("${OPENSSL_ROOT_DIR}" STREQUAL "" AND APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(OPENSSL_ROOT_DIR /usr/local/opt/openssl)
        endif()
        find_package(OpenSSL REQUIRED)
    endif()

    include(ExternalProject)
    string(REPLACE ";" "|" _iris_qca_prefix_path "${CMAKE_PREFIX_PATH}")
    string(REPLACE ";" "|" _iris_qca_osx_architectures "${CMAKE_OSX_ARCHITECTURES}")
    string(REPLACE ";" "|" _iris_qca_find_root_path "${CMAKE_FIND_ROOT_PATH}")
    set(QCA_BUILD_OPTIONS
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_PLUGINS=ossl
        -DLOAD_SHARED_PLUGINS=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_TOOLS=OFF
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${QCA_PREFIX}
        -DCMAKE_PREFIX_PATH=${_iris_qca_prefix_path}
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

    if(EXISTS "${QCA_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "QCA: found local sources at ${QCA_SOURCE_DIR}")
        ExternalProject_Add(QcaProject
            PREFIX ${QCA_PREFIX}
            BINARY_DIR ${QCA_BUILD_DIR}
            SOURCE_DIR ${QCA_SOURCE_DIR}
            LIST_SEPARATOR "|"
            CMAKE_ARGS ${QCA_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${Qca_LIBRARY}
            INSTALL_COMMAND "")
    else()
        include(FindGit)
        find_package(Git)
        if(NOT Git_FOUND)
            message(FATAL_ERROR "Git not found! Bundled QCA needs Git")
        endif()
        ExternalProject_Add(QcaProject
            PREFIX ${QCA_PREFIX}
            BINARY_DIR ${QCA_BUILD_DIR}
            GIT_REPOSITORY ${IRIS_BUNDLED_QCA_GIT_REPOSITORY}
            GIT_TAG ${IRIS_BUNDLED_QCA_GIT_TAG}
            LIST_SEPARATOR "|"
            CMAKE_ARGS ${QCA_BUILD_OPTIONS}
            BUILD_BYPRODUCTS ${Qca_LIBRARY}
            INSTALL_COMMAND ""
            UPDATE_COMMAND "")
    endif()
else()
    message(WARNING "Disabling IRIS_BUNDLED_QCA makes DTLS/PsiMedia support dependent on the system QCA build")
    message(STATUS "QCA: using system")
    find_package(Qca REQUIRED)
endif()
