set(QCA_VERSION "" CACHE STRING "QCA version override (for example 3.0.11 or v3.0.11)")
set(qca_version "")

if(QCA_VERSION)
    set(qca_version "${QCA_VERSION}")
    string(REGEX REPLACE "^v" "" qca_version "${qca_version}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.version")
    # Generated source trees and shallow clones are self-contained: when a
    # .version file is present, determining the version must not require Git.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.version" qca_version)
    string(STRIP "${qca_version}" qca_version)
    string(REGEX REPLACE "^v" "" qca_version "${qca_version}")
else()
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE git_version_result
            OUTPUT_VARIABLE git_version_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(git_version_result EQUAL 0
           AND git_version_tag MATCHES "^v[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
            string(REGEX REPLACE "^v" "" qca_version "${git_version_tag}")
        endif()
    endif()

    # git archive (including GitHub source downloads) substitutes the tag in
    # this tracked file. In a checkout its literal $Format placeholder is
    # ignored; Git remains the source of version information there.
    if(NOT qca_version AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.archive-version")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.archive-version" archive_version_tag)
        string(STRIP "${archive_version_tag}" archive_version_tag)
        if(archive_version_tag MATCHES "^v[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
            string(REGEX REPLACE "^v" "" qca_version "${archive_version_tag}")
        endif()
    endif()
endif()

if(NOT qca_version)
    message(FATAL_ERROR
            "Cannot determine QCA version: set QCA_VERSION, provide .version, or use a Git checkout/source archive with a reachable v* tag")
endif()

if(NOT qca_version MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
    message(FATAL_ERROR "Invalid QCA version '${qca_version}'")
endif()
