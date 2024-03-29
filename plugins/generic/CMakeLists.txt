cmake_minimum_required(VERSION 3.10.0)

set( plugins_list
    attentionplugin
    autoreplyplugin
    birthdayreminderplugin
    chessplugin
    cleanerplugin
    clientswitcherplugin
    conferenceloggerplugin
    contentdownloaderplugin
    enummessagesplugin
    extendedmenuplugin
    extendedoptionsplugin
    gomokugameplugin
    historykeeperplugin
    imageplugin
    imagepreviewplugin
    jabberdiskplugin
    juickplugin
    messagefilterplugin
    omemoplugin
    openpgpplugin
    otrplugin
    pepchangenotifyplugin
    qipxstatusesplugin
    skinsplugin
    stopspamplugin
    storagenotesplugin
    translateplugin
    watcherplugin
)

if(NOT APPLE AND (NOT HAIKU) AND (WIN32 OR USE_X11))
    list(APPEND plugins_list
        videostatusplugin
    )
endif()

if(BUILD_PSIMEDIA AND (EXISTS "${CMAKE_CURRENT_LIST_DIR}/psimedia"))
    list(APPEND plugins_list
        psimedia
    )
endif()

message(STATUS "Plugins from generic subdirectory")
# If BUILD_PLUGINS variable was set as "-plugin1;-plugin2"
# We proceed all plugins with plugin1 and plugin2 excluded
string(REGEX MATCH ".*[-]+" DISABLED_PLUGINS "${BUILD_PLUGINS}")
if(NOT "${DISABLED_PLUGINS}" STREQUAL "")
    set(plugins ${plugins_list})
    foreach(mp ${BUILD_PLUGINS})
        string(SUBSTRING "${mp}" 0 1 FIRST_LETTER)
        string(SUBSTRING ${mp} 1 -1 PLUGIN_BODY)
        if(${FIRST_LETTER} STREQUAL "-")
            message(STATUS "Exclude subdirectory: ${PLUGIN_BODY}")
            list(REMOVE_ITEM plugins "${PLUGIN_BODY}")
        endif()
    endforeach()
else()
    if( "${BUILD_PLUGINS}" STREQUAL "ALL" )
        set ( plugins ${plugins_list} )
    else()
        set ( plugins "${BUILD_PLUGINS}" )
    endif()
endif()

foreach(plugin ${plugins_list})
    foreach(subdir ${plugins})
        if( ${plugin} STREQUAL ${subdir} )
            message(STATUS "Add subdirectory: ${plugin}")
            add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/${plugin}")
        endif()
    endforeach()
endforeach()
