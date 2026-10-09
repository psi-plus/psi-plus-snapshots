set(IRIS_VERSION "" CACHE STRING "Iris version override (for example 1.1 or v1.1)")
set(iris_version "")

if(IRIS_VERSION)
    set(iris_version "${IRIS_VERSION}")
    string(REGEX REPLACE "^v" "" iris_version "${iris_version}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.version")
    # Generated source trees and shallow clones are self-contained: when a
    # .version file is present, determining the version must not require Git.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.version" iris_version)
    string(STRIP "${iris_version}" iris_version)
    string(REGEX REPLACE "^v" "" iris_version "${iris_version}")
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
            string(REGEX REPLACE "^v" "" iris_version "${git_version_tag}")
        endif()
    endif()

    # git archive (including GitHub source downloads) substitutes the tag in
    # this tracked file. In a checkout its literal $Format placeholder is
    # ignored; Git remains the source of version information there.
    if(NOT iris_version AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.archive-version")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.archive-version" archive_version_tag)
        string(STRIP "${archive_version_tag}" archive_version_tag)
        if(archive_version_tag MATCHES "^v[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
            string(REGEX REPLACE "^v" "" iris_version "${archive_version_tag}")
        endif()
    endif()
endif()

if(NOT iris_version)
    message(FATAL_ERROR
            "Cannot determine Iris version: set IRIS_VERSION, provide .version, or use a Git checkout/source archive with a reachable v* tag")
endif()

if(NOT iris_version MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
    message(FATAL_ERROR "Invalid Iris version '${iris_version}'")
endif()
