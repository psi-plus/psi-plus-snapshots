cmake_minimum_required(VERSION 3.19)

project(jdns
    VERSION 3.1.1
    LANGUAGES C CXX
)

include(GNUInstallDirs)

# On Windows debug libraries use the conventional 'd' postfix. Keep the
# historical macOS postfix for compatibility with existing consumers.
if(WIN32)
    set(CMAKE_DEBUG_POSTFIX "d")
elseif(APPLE)
    set(CMAKE_DEBUG_POSTFIX "_debug")
endif()

# Static is the embedding-friendly default. Distribution packages that need
# shared libraries opt in with -DBUILD_SHARED_LIBS=ON. Static PIC builds are
# controlled independently by the standard CMAKE_POSITION_INDEPENDENT_CODE.
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BUILD_QJDNS "Build the JDNS Qt wrapper" ON)
option(JDNS_BUILD_TESTS "Build QJDns transport tests" OFF)
option(JDNS_ENABLE_INSTALL "Generate JDNS installation rules" ON)

if(ANDROID)
    set(_jdns_build_tool_default OFF)
else()
    set(_jdns_build_tool_default ON)
endif()
option(BUILD_JDNS_TOOL "Build the jdns command-line tool" ${_jdns_build_tool_default})
unset(_jdns_build_tool_default)

if(NOT BUILD_QJDNS)
    set(BUILD_JDNS_TOOL OFF)
endif()

if(BUILD_QJDNS)
    set(JDNS_QT_MAJOR_VERSION "6" CACHE STRING "Qt major version used by QJDns (5 or 6)")
    set_property(CACHE JDNS_QT_MAJOR_VERSION PROPERTY STRINGS 5 6)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        option(MULTI_QT "Add the Qt major version suffix to QJDns artifacts" ON)
    else()
        option(MULTI_QT "Add the Qt major version suffix to QJDns artifacts" OFF)
    endif()

    if(JDNS_QT_MAJOR_VERSION STREQUAL "6")
        find_package(Qt6 REQUIRED COMPONENTS Core Network)
        set(QJDNS_QT_TARGETS Qt6::Core Qt6::Network)
        set(QJDns_QT_PC_VERSION "Qt6Core Qt6Network")
        set(QT_MAJ "-qt6")
    elseif(JDNS_QT_MAJOR_VERSION STREQUAL "5")
        find_package(Qt5 REQUIRED COMPONENTS Core Network)
        set(QJDNS_QT_TARGETS Qt5::Core Qt5::Network)
        set(QJDns_QT_PC_VERSION "Qt5Core Qt5Network")
        set(QT_MAJ "-qt5")
    else()
        message(FATAL_ERROR "JDNS_QT_MAJOR_VERSION must be 5 or 6")
    endif()

    if(NOT MULTI_QT)
        set(QT_MAJ "")
    endif()
endif()

set(CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE ON)
set(JDNS_INCLUDEDIR "${CMAKE_CURRENT_SOURCE_DIR}/include/jdns")

set(LIB_SUFFIX "" CACHE STRING "Suffix appended to the library directory name")
set(BIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}" CACHE PATH "Binary install directory")
set(LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}${LIB_SUFFIX}" CACHE PATH "Library install directory")
set(INCLUDE_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}" CACHE PATH "Header install directory")
set(DOC_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/doc/jdns" CACHE PATH "Documentation install directory")

set(LIB_INSTALL_DIR_FULL "${LIB_INSTALL_DIR}")
set(INCLUDE_INSTALL_DIR_FULL "${INCLUDE_INSTALL_DIR}")

# Normalize paths before deciding whether install locations can be represented
# relative to CMAKE_INSTALL_PREFIX. This keeps Windows packages relocatable.
get_filename_component(CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" ABSOLUTE)
get_filename_component(BIN_INSTALL_DIR "${BIN_INSTALL_DIR}" ABSOLUTE)
get_filename_component(LIB_INSTALL_DIR "${LIB_INSTALL_DIR}" ABSOLUTE)
get_filename_component(INCLUDE_INSTALL_DIR "${INCLUDE_INSTALL_DIR}" ABSOLUTE)
get_filename_component(DOC_INSTALL_DIR "${DOC_INSTALL_DIR}" ABSOLUTE)

if(WIN32)
    set(USE_RELATIVE_PATHS ON)
    foreach(_path IN ITEMS "${BIN_INSTALL_DIR}" "${LIB_INSTALL_DIR}" "${INCLUDE_INSTALL_DIR}")
        string(FIND "${_path}/" "${CMAKE_INSTALL_PREFIX}/" _position)
        if(NOT _position EQUAL 0)
            set(USE_RELATIVE_PATHS OFF)
        endif()
    endforeach()
else()
    # Relative install names are unsuitable for macOS shared libraries.
    set(USE_RELATIVE_PATHS OFF)
endif()

if(USE_RELATIVE_PATHS)
    file(RELATIVE_PATH BIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}" "${BIN_INSTALL_DIR}")
    file(RELATIVE_PATH LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}" "${LIB_INSTALL_DIR}")
    file(RELATIVE_PATH INCLUDE_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}" "${INCLUDE_INSTALL_DIR}")
    file(RELATIVE_PATH DOC_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}" "${DOC_INSTALL_DIR}")
endif()

if(NOT MSVC)
    set(JDNS_CONFIG_INSTALL_DIR "${LIB_INSTALL_DIR}/cmake/jdns")
    set(QJDNS_CONFIG_INSTALL_DIR "${LIB_INSTALL_DIR}/cmake/qjdns${QT_MAJ}")
    set(QJDNS_COMMON_CONFIG_INSTALL_DIR "${LIB_INSTALL_DIR}/cmake/qjdns")
else()
    set(JDNS_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/cmake")
    set(QJDNS_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/cmake")
    set(QJDNS_COMMON_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/cmake")
endif()

set(JDNS_LIB_MAJOR_VERSION "${PROJECT_VERSION_MAJOR}")
set(JDNS_LIB_MINOR_VERSION "${PROJECT_VERSION_MINOR}")
set(JDNS_LIB_PATCH_VERSION "${PROJECT_VERSION_PATCH}")
set(JDNS_LIB_VERSION_STRING "${PROJECT_VERSION}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(JDNS_ENABLE_INSTALL AND NOT WIN32)
    configure_file(jdns.pc.in jdns.pc @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/jdns.pc"
        DESTINATION "${LIB_INSTALL_DIR}/pkgconfig")

    if(BUILD_QJDNS)
        configure_file(qjdns.pc.in "qjdns${QT_MAJ}.pc" @ONLY)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/qjdns${QT_MAJ}.pc"
            DESTINATION "${LIB_INSTALL_DIR}/pkgconfig")
    endif()
endif()

add_subdirectory(src)

if(JDNS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_JDNS_TOOL)
    add_subdirectory(tools/jdns)
endif()

if(JDNS_ENABLE_INSTALL)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        @ONLY
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        JDnsConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/JDnsConfig.cmake"
        INSTALL_DESTINATION "${JDNS_CONFIG_INSTALL_DIR}"
        PATH_VARS BIN_INSTALL_DIR LIB_INSTALL_DIR INCLUDE_INSTALL_DIR
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/JDnsConfigVersion.cmake"
        VERSION "${JDNS_LIB_VERSION_STRING}"
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/JDnsConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/JDnsConfigVersion.cmake"
        DESTINATION "${JDNS_CONFIG_INSTALL_DIR}"
    )
    install(EXPORT jdns-export
        DESTINATION "${JDNS_CONFIG_INSTALL_DIR}"
        FILE JDnsTargets.cmake
    )

    if(BUILD_QJDNS)
        install(EXPORT qjdns-export
            DESTINATION "${QJDNS_CONFIG_INSTALL_DIR}"
            FILE "QJDns${QT_MAJ}Targets.cmake"
        )

        configure_package_config_file(
            QJDns-qtConfig.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/QJDns${QT_MAJ}Config.cmake"
            INSTALL_DESTINATION "${QJDNS_CONFIG_INSTALL_DIR}"
        )
        write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/QJDns${QT_MAJ}ConfigVersion.cmake"
            VERSION "${JDNS_LIB_VERSION_STRING}"
            COMPATIBILITY SameMajorVersion
        )
        install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/QJDns${QT_MAJ}Config.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/QJDns${QT_MAJ}ConfigVersion.cmake"
            DESTINATION "${QJDNS_CONFIG_INSTALL_DIR}"
        )

        if(MULTI_QT)
            configure_file(QJDnsConfig.cmake.in QJDnsConfig.cmake @ONLY)
            write_basic_config_version_file(
                "${CMAKE_CURRENT_BINARY_DIR}/QJDnsConfigVersion.cmake"
                VERSION "${JDNS_LIB_VERSION_STRING}"
                COMPATIBILITY SameMajorVersion
            )
            install(FILES
                "${CMAKE_CURRENT_BINARY_DIR}/QJDnsConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/QJDnsConfigVersion.cmake"
                DESTINATION "${QJDNS_COMMON_CONFIG_INSTALL_DIR}"
            )
        endif()
    endif()

    add_custom_target(uninstall
        "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    )
endif()

find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    configure_file(Doxyfile.in Doxyfile @ONLY)
    add_custom_target(doc
        "${DOXYGEN_EXECUTABLE}" "${CMAKE_BINARY_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
    if(JDNS_ENABLE_INSTALL)
        install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/apidocs/html"
            DESTINATION "${DOC_INSTALL_DIR}"
            OPTIONAL
        )
    endif()
endif()
