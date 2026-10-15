# Install-time part of PsimediaGStreamerRuntime.cmake. Inputs: psimedia_gst_required, psimedia_gst_optional (lists
# of element:plugin), psimedia_gst_bin_dir, psimedia_gst_plugin_dir (GStreamer SDK), psimedia_gst_dest (install
# directory), psimedia_gst_media_plugin (the built plugin DLL). Install scripts share one scope with the other
# install rules (whose if() conditions name components such as "plugins"), so every variable here is prefixed.

if(NOT IS_ABSOLUTE "${psimedia_gst_dest}")
    set(psimedia_gst_dest "${CMAKE_INSTALL_PREFIX}/${psimedia_gst_dest}")
endif()
if(DEFINED ENV{DESTDIR})
    set(psimedia_gst_dest "$ENV{DESTDIR}${psimedia_gst_dest}")
endif()

set(psimedia_gst_manifest "# GStreamer runtime installed for psimedia\n# element plugin status\n")
set(psimedia_gst_plugins)
foreach(psimedia_gst_kind psimedia_gst_required psimedia_gst_optional)
    foreach(psimedia_gst_entry IN LISTS ${psimedia_gst_kind})
        string(REPLACE ":" ";" psimedia_gst_pair "${psimedia_gst_entry}")
        list(GET psimedia_gst_pair 0 psimedia_gst_element)
        list(GET psimedia_gst_pair 1 psimedia_gst_plugin)
        if(psimedia_gst_element STREQUAL "-")
            set(psimedia_gst_element "(${psimedia_gst_plugin})")
        endif()
        set(psimedia_gst_file "${psimedia_gst_plugin_dir}/gst${psimedia_gst_plugin}.dll")
        if(EXISTS "${psimedia_gst_file}")
            list(APPEND psimedia_gst_plugins "${psimedia_gst_file}")
            string(APPEND psimedia_gst_manifest "${psimedia_gst_element} ${psimedia_gst_plugin} installed\n")
        elseif(psimedia_gst_kind STREQUAL "psimedia_gst_required")
            message(FATAL_ERROR "GStreamer plugin ${psimedia_gst_plugin} (element ${psimedia_gst_element}) "
                                "is missing in ${psimedia_gst_plugin_dir}")
        else()
            message(STATUS "Optional GStreamer plugin ${psimedia_gst_plugin} (element ${psimedia_gst_element}) "
                           "is not in the SDK")
            string(APPEND psimedia_gst_manifest "${psimedia_gst_element} ${psimedia_gst_plugin} absent\n")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES psimedia_gst_plugins)

if(NOT CMAKE_HOST_WIN32)
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
endif()
file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES ${psimedia_gst_plugins} "${psimedia_gst_media_plugin}"
    RESOLVED_DEPENDENCIES_VAR psimedia_gst_found
    UNRESOLVED_DEPENDENCIES_VAR psimedia_gst_unresolved
    DIRECTORIES "${psimedia_gst_bin_dir}"
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-")
# Only the SDK provides the runtime: system DLLs and the Universal CRT stay out of the package.
file(TO_CMAKE_PATH "${psimedia_gst_bin_dir}" psimedia_gst_sdk_bin)
string(TOLOWER "${psimedia_gst_sdk_bin}" psimedia_gst_sdk_bin)
set(psimedia_gst_resolved)
foreach(psimedia_gst_file IN LISTS psimedia_gst_found)
    file(TO_CMAKE_PATH "${psimedia_gst_file}" psimedia_gst_path)
    get_filename_component(psimedia_gst_directory "${psimedia_gst_path}" DIRECTORY)
    string(TOLOWER "${psimedia_gst_directory}" psimedia_gst_directory)
    if(psimedia_gst_directory STREQUAL psimedia_gst_sdk_bin)
        list(APPEND psimedia_gst_resolved "${psimedia_gst_file}")
    endif()
endforeach()
set(psimedia_gst_sdk_unresolved)
foreach(psimedia_gst_name IN LISTS psimedia_gst_unresolved)
    # Unresolved names are system DLLs unless the SDK has them.
    if(EXISTS "${psimedia_gst_bin_dir}/${psimedia_gst_name}")
        list(APPEND psimedia_gst_sdk_unresolved "${psimedia_gst_name}")
    endif()
endforeach()
if(psimedia_gst_sdk_unresolved)
    message(FATAL_ERROR "Could not resolve GStreamer runtime DLLs: ${psimedia_gst_sdk_unresolved}")
endif()

file(INSTALL ${psimedia_gst_plugins} DESTINATION "${psimedia_gst_dest}/gstreamer-1.0")
file(INSTALL ${psimedia_gst_resolved} DESTINATION "${psimedia_gst_dest}")

string(APPEND psimedia_gst_manifest "# runtime DLLs\n")
list(SORT psimedia_gst_resolved)
foreach(psimedia_gst_file IN LISTS psimedia_gst_resolved)
    get_filename_component(psimedia_gst_name "${psimedia_gst_file}" NAME)
    string(APPEND psimedia_gst_manifest "${psimedia_gst_name}\n")
endforeach()
file(WRITE "${psimedia_gst_dest}/gstreamer-runtime.txt" "${psimedia_gst_manifest}")
list(LENGTH psimedia_gst_plugins psimedia_gst_plugin_count)
list(LENGTH psimedia_gst_resolved psimedia_gst_dll_count)
message(STATUS "Installed GStreamer runtime: ${psimedia_gst_plugin_count} plugins, ${psimedia_gst_dll_count} DLLs")
