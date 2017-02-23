unset(MOC_SOURCES)
unset(HEADERS)
unset(PLAIN_HEADERS)
unset(FORMS)
unset(SOURCES)
unset(PLAIN_SOURCES)
unset(UI_FORMS)
unset(EXTRA_LDFLAGS)

include_directories(libpsi/dialogs)

list(APPEND HEADERS
	grepshortcutkeydialog.h
	)

list(APPEND PLAIN_SOURCES
	grepshortcutkeydialog.cpp
	)

list(APPEND FORMS
	grepshortcutkeydialog.ui
	)

qt4_wrap_ui(UI_FORMS ${FORMS})
qt_wrap_cpp(MOC_SOURCES ${HEADERS})
add_library(libpsi_dialogs STATIC ${HEADERS} ${MOC_SOURCES} ${UI_FORMS} ${PLAIN_SOURCES} ${PLAIN_HEADERS})
target_link_libraries(libpsi_dialogs ${QT_LIBRARIES})
target_include_directories(libpsi_dialogs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
