cmake_minimum_required(VERSION 3.2.0)

set(VER_FILE ${PROJECT_SOURCE_DIR}/version)
unset(APP_VERSION)
unset(PSI_REVISION)
unset(PSI_PLUS_REVISION)
set(DEFAULT_VER "1.5")

if(NOT Git_FOUND)
    include(FindGit)
    find_package(Git)
    if((NOT BUNDLED_USRSCTP OR NOT BUNDLED_QCA) AND (NOT Git_FOUND))
        message(STATUS "Git utility not found")
    endif()
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/generate-single-repo.sh")
    set(IS_SNAPSHOT ON)
endif()

function(read_version_file VF_RESULT)
    if(EXISTS "${VER_FILE}")
        message(STATUS "Found Psi version file: ${VER_FILE}")
        file(STRINGS "${VER_FILE}" VER_LINES)
        if(VER_LINES)
            set(${VF_RESULT} ${VER_LINES} PARENT_SCOPE)
        else()
            message(FATAL_ERROR "Can't read ${VER_FILE} contents")
        endif()
        unset(VER_LINES)
    else()
        message(FATAL_ERROR "${VER_FILE} not found")
    endif()
endfunction()

function(run_git GIT_ARG1 GIT_ARG2 GIT_ARG3 RESULT)
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} ${GIT_ARG1} ${GIT_ARG2} ${GIT_ARG3}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE RES
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE ERROR_1
            )
        if(RES)
            set(${RESULT} ${RES} PARENT_SCOPE)
        elseif(ERROR_1)
            message(WARNING "Can't execute ${GIT_EXECUTABLE} ${GIT_ARG1} ${GIT_ARG2} ${GIT_ARG3}: ${ERROR_1}")
        endif()
    endif()
endfunction()

function(obtain_git_version GIT_VERSION GIT_FULL_VERSION)
    if(EXISTS "${PROJECT_SOURCE_DIR}/\.git" AND (NOT IS_SNAPSHOT))
        run_git("describe" "--tags" "--abbrev=0" MAIN_VER)
        if(MAIN_VER)
            set(APP_VERSION "${MAIN_VER}" PARENT_SCOPE)
            set(${GIT_VERSION} ${MAIN_VER} PARENT_SCOPE)
            run_git("rev-list" "--count" "${MAIN_VER}\.\.HEAD" COMMITS)
            if(COMMITS)
                run_git("rev-parse" "--short" "HEAD" VER_HASH)
                if(COMMITS AND VER_HASH)
                    set(PSI_REVISION ${VER_HASH} PARENT_SCOPE)
                    set(APP_VERSION "${MAIN_VER}.${COMMITS}" PARENT_SCOPE)
                    set(${GIT_FULL_VERSION} "${MAIN_VER}.${COMMITS} \(${PSI_COMPILATION_DATE}, ${VER_HASH}${PSI_VER_SUFFIX}\)" PARENT_SCOPE)
                endif()
            endif()
        endif()
    elseif(IS_SNAPSHOT)
        message(STATUS "${PROJECT_NAME} snapshot detected. Reading version from file...")
    else()
        message(STATUS "${PROJECT_SOURCE_DIR}/\.git directory not found. Reading version from file...")
    endif()
endfunction()

function(obtain_psi_file_version VERSION_LINES OUTPUT_VERSION)
    string(REGEX MATCHALL "^([0-9]+(\\.[0-9]+)+)+(.+, ([a-fA-F0-9]+)?)?" VER_LINE_A ${VERSION_LINES})
    if(${CMAKE_MATCH_COUNT} EQUAL 4)
        if(CMAKE_MATCH_1)
            set(_APP_VERSION ${CMAKE_MATCH_1})
        endif()
        if(CMAKE_MATCH_4)
            set(_PSI_REVISION ${CMAKE_MATCH_4})
        endif()
    elseif(${CMAKE_MATCH_COUNT} EQUAL 2)
        if(CMAKE_MATCH_1)
            set(_APP_VERSION ${CMAKE_MATCH_1})
        endif()
    endif()
    if(_APP_VERSION)
        set(APP_VERSION ${_APP_VERSION} PARENT_SCOPE)
        message(STATUS "Psi version found: ${_APP_VERSION}")
    else()
        message(WARNING "Psi+ version not found! ${DEFAULT_VER} version will be set")
        set(_APP_VERSION ${DEFAULT_VER})
        set(APP_VERSION ${_APP_VERSION} PARENT_SCOPE)
    endif()
    if(_PSI_REVISION)
        set(PSI_REVISION ${_PSI_REVISION} PARENT_SCOPE)
        message(STATUS "Psi revision found: ${_PSI_REVISION}")
    endif()
    if(_APP_VERSION AND _PSI_REVISION)
        set(${OUTPUT_VERSION} "${_APP_VERSION} \(${PSI_COMPILATION_DATE}, ${_PSI_REVISION}${PSI_VER_SUFFIX}\)" PARENT_SCOPE)
    else()
        if(PRODUCTION)
            set(${OUTPUT_VERSION} "${_APP_VERSION}" PARENT_SCOPE)
        else()
            set(${OUTPUT_VERSION} "${_APP_VERSION} \(${PSI_COMPILATION_DATE}${PSI_VER_SUFFIX}\)" PARENT_SCOPE)
        endif()
    endif()
endfunction()

if(NOT PSI_VERSION)
    obtain_git_version(PSIVER GITVER)
    if(PSIVER AND GITVER)
        message(STATUS "Git Version ${PSIVER}")
        if(PRODUCTION)
            set(PSI_VERSION "${PSIVER}")
        else()
            set(PSI_VERSION "${GITVER}")
        endif()
    else()
        read_version_file(VER_LINES)
        obtain_psi_file_version("${VER_LINES}" FILEVER)
        if(FILEVER)
            set(PSI_VERSION "${FILEVER}")
        endif()
    endif()
    unset(VER_LINES)
endif()

message(STATUS "${CLIENT_NAME} version set: ${PSI_VERSION}")
