include_guard(GLOBAL)

function(make_patch_command out_patch_command)
    # required args
    set(oneValueArgs
        SOURCE_DIR
        PATCH_FILE
        GIT_EXECUTABLE
        PATCH_APPLY_PATH
    )
    set(multiValueArgs
        CHECKOUT_FILES
    )
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT out_patch_command)
        message(FATAL_ERROR "out_patch_command must be provided")
    endif()

    foreach(v IN ITEMS SOURCE_DIR PATCH_FILE GIT_EXECUTABLE PATCH_APPLY_PATH)
        if(NOT ARG_${v})
            message(FATAL_ERROR "Missing required argument: ${v}")
        endif()
    endforeach()

    if(NOT ARG_CHECKOUT_FILES)
        # checkout is not required, but then git apply may run into problems with the files being incorrect
        # therefore, it is better for the user to explicitly pass
        message(FATAL_ERROR "CHECKOUT_FILES must be provided")
    endif()

    # create a script in build-dir to avoid dependency on the shell.
    set(script_path "${CMAKE_CURRENT_BINARY_DIR}/_apply_patch_${out_patch_command}.cmake")

    # generate a script with a "binding" to arguments.
    # SOURCE_DIR / PATCH_APPLY_PATH are left as string tokens (including <SOURCE_DIR>).
    file(WRITE "${script_path}" [=[
    if(NOT DEFINED SOURCE_DIR)
        message(FATAL_ERROR "SOURCE_DIR is not set")
    endif()

    if(NOT DEFINED GIT_EXECUTABLE)
        message(FATAL_ERROR "GIT_EXECUTABLE is not set")
    endif()

    if(NOT DEFINED PATCH_FILE)
        message(FATAL_ERROR "PATCH_FILE is not set")
    endif()

    if(NOT DEFINED PATCH_APPLY_PATH)
        message(FATAL_ERROR "PATCH_APPLY_PATH is not set")
    endif()

    # 1) copy the patch to SOURCE_DIR
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PATCH_FILE}" "${SOURCE_DIR}/"
    )

    # 2) checkout the required files (paths within the repository relative to SOURCE_DIR)
    ]=])

    foreach(f IN LISTS ARG_CHECKOUT_FILES)
        # In the script, generate checkout lines for each file.
        file(APPEND "${script_path}" "execute_process(\n")
        file(APPEND "${script_path}" "  COMMAND \"\${GIT_EXECUTABLE}\" checkout \"\${SOURCE_DIR}/${f}\"\n")
        file(APPEND "${script_path}" "  WORKING_DIRECTORY \"\${SOURCE_DIR}\"\n")
        file(APPEND "${script_path}" "  RESULT_VARIABLE rc_checkout_${f}\n")
        file(APPEND "${script_path}" ")\n")
    endforeach()

    file(APPEND "${script_path}" [=[
    # 3) apply the patch
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_APPLY_PATH}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE rc_apply
    )

    if(NOT rc_apply EQUAL 0)
        # Usually it's "already applied" or a conflict
        message(STATUS "already patched (git apply rc=${rc_apply})")
    endif()
    ]=])

    # Return PATCH_COMMAND as a script call with -D parameters
    set(${out_patch_command}
        "${CMAKE_COMMAND}" -DSOURCE_DIR=${ARG_SOURCE_DIR}
        -DPATCH_FILE=${ARG_PATCH_FILE}
        -DGIT_EXECUTABLE=${ARG_GIT_EXECUTABLE}
        -DPATCH_APPLY_PATH=${ARG_PATCH_APPLY_PATH}
        -P "${script_path}"
        PARENT_SCOPE
    )
endfunction()
