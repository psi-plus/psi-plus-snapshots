unset(HEADERS)
unset(FORMS)
unset(SOURCES)
unset(UI_FORMS)

get_filename_component(ABS_PARENT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include_directories(${ABS_PARENT_DIR})

list(APPEND HEADERS
    optionsdlg.h
    opt_status.h
    opt_statuspep.h
    opt_toolbars.h
    opt_shortcuts.h
    opt_statusauto.h
    opt_iconset.h
    optionsdlgbase.h
    opt_appearance.h
    opt_advanced.h
    opt_events.h
    opt_pluginwrapper.h
    opt_popups.h
    opt_roster.h
    opt_roster_main.h
    opt_roster_muc.h
    opt_groupchat.h
    opt_sound.h
    opt_messages.h
    opt_messages_common.h
    opt_chat.h
    optionstab.h
    opt_tree.h
    opt_application.h
    opt_plugins.h
    opt_input.h
    opt_statusgeneral.h
    opt_theme.h
    opt_accounts.h
)

list(APPEND SOURCES

    optionsdlgbase.cpp
    optionstab.cpp
    opt_plugins.cpp

    optionsdlg.cpp
    opt_advanced.cpp
    opt_appearance.cpp
    opt_application.cpp
    opt_chat.cpp
    opt_events.cpp
    opt_groupchat.cpp
    opt_iconset.cpp
    opt_messages.cpp
    opt_messages_common.cpp
    opt_pluginwrapper.cpp
    opt_popups.cpp
    opt_roster.cpp
    opt_roster_main.cpp
    opt_roster_muc.cpp
    opt_shortcuts.cpp
    opt_sound.cpp
    opt_input.cpp
    opt_status.cpp
    opt_statusauto.cpp
    opt_statusgeneral.cpp
    opt_statuspep.cpp
    opt_theme.cpp
    opt_toolbars.cpp
    opt_tree.cpp
    opt_accounts.cpp
)

list(APPEND FORMS
    opt_advanced.ui
    opt_appearance.ui
    opt_appearance_misc.ui
    opt_application.ui
    opt_chat.ui
    opt_events.ui
    opt_general_groupchat.ui
    opt_iconset_activity.ui
    opt_iconset_affiliation.ui
    opt_iconset_client.ui
    opt_iconset_emo.ui
    opt_iconset_mood.ui
    opt_iconset_roster.ui
    opt_iconset_system.ui
    opt_input.ui
    opt_lookfeel_toolbars.ui
    opt_messages_common.ui
    opt_plugins.ui
    opt_popups.ui
    opt_roster_main.ui
    opt_roster_muc.ui
    opt_shortcuts.ui
    opt_sound.ui
    opt_statusauto.ui
    opt_statusgeneral.ui
    opt_statuspep.ui
    opt_theme.ui
    plugininfodialog.ui
    ui_isdetails.ui
    ui_options.ui
)

qt_wrap_ui(UI_FORMS ${FORMS})
add_library(options STATIC ${SOURCES} ${HEADERS} ${UI_FORMS})
target_link_libraries(options ${QT_LIBRARIES} tools widgets tabs)
target_include_directories(options PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
