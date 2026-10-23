cmake_minimum_required(VERSION 3.11)

if(NOT DEFINED QCA_SOURCE_DIR OR NOT IS_DIRECTORY "${QCA_SOURCE_DIR}")
    message(FATAL_ERROR "QCA source tree is unavailable for macOS warning fixes")
endif()

# The upstream fixes are also tracked at psi-im/qca#25. This patch is
# intentionally idempotent: ExternalProject can repeat its patch step.
function(replace_deprecated_qca_macro path old_text new_text)
    set(source "${QCA_SOURCE_DIR}/${path}")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Expected locked QCA source missing: ${source}")
    endif()
    file(READ "${source}" content)
    string(FIND "${content}" "${old_text}" old_pos)
    if(NOT old_pos EQUAL -1)
        string(REPLACE "${old_text}" "${new_text}" content "${content}")
        file(WRITE "${source}" "${content}")
    else()
        string(FIND "${content}" "${new_text}" new_pos)
        if(new_pos EQUAL -1)
            message(FATAL_ERROR "Locked QCA source changed unexpectedly: ${path}")
        endif()
    endif()
endfunction()

replace_deprecated_qca_macro(
    "src/qca_securefile.cpp"
    "#if (defined(Q_OS_MACOS) || defined(Q_OS_MAC)) && defined(F_NOCACHE)"
    "#if defined(Q_OS_MACOS) && defined(F_NOCACHE)")
replace_deprecated_qca_macro(
    "plugins/qca-ossl/basetlscontext.cpp"
    "#ifdef Q_OS_MAC\n"
    "#ifdef Q_OS_MACOS\n")
