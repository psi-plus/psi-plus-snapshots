unset(MOC_SOURCES)
unset(HEADERS)
unset(PLAIN_HEADERS)
unset(FORMS)
unset(SOURCES)
unset(PLAIN_SOURCES)
unset(UI_FORMS)
unset(EXTRA_LDFLAGS)

if(APPLE)
	# mac {
	# 	# Growl
	# 	contains(DEFINES, HAVE_GROWL) {
	# 		include($$PWD/growlnotifier/growlnotifier.pri)
	# 	}

	# 	# Mac dock
	# 	include($$PWD/mac_dock/mac_dock.pri)
	# }
endif()

list(APPEND HEADERS
	# tools
	priorityvalidator.h

	# idle
	idle/idle.h

	# systemwatch
	systemwatch/systemwatch.h

	# globalshortcut
	globalshortcut/globalshortcuttrigger.h
	)

list(APPEND PLAIN_HEADERS
	# tools
	maybe.h
	iodeviceopener.h

	# atomicxmlfile
	atomicxmlfile/atomicxmlfile.h

	# globalshortcut
	globalshortcut/globalshortcutmanager.h

	# simplecli
	simplecli/simplecli.h

	# spellchecker
	spellchecker/spellchecker.h
	spellchecker/spellhighlighter.h
	)

list(APPEND PLAIN_SOURCES
	# tools
	priorityvalidator.cpp

	# spellchecker
	spellchecker/spellchecker.cpp
	spellchecker/spellhighlighter.cpp

	# tools
	iodeviceopener.cpp

	# idle
	idle/idle.cpp

	# atomicxmlfile
	atomicxmlfile/atomicxmlfile.cpp

	# globalshortcut
	globalshortcut/globalshortcutmanager.cpp

	# systemwatch
	systemwatch/systemwatch.cpp

	# simplecli
	simplecli/simplecli.cpp
	)

if(APPLE)
	list(APPEND HEADERS
		# systemwatch
		systemwatch/systemwatch_mac.h

		# globalshortcut
		globalshortcut/NDKeyboardLayout.h

		# spellchecker
		spellchecker/macspellchecker.h
		)

	list(APPEND SOURCES
		#idle
		idle/idle_mac.cpp

		# systemwatch
		systemwatch/systemwatch_mac.cpp

		# globalshortcut
		# *g++*:QMAKE_OBJECTIVE_CFLAGS += -std=gnu99
		globalshortcut/$$PWD/globalshortcutmanager_mac.mm
		globalshortcut/NDKeyboardLayout.m

		# spellchecker
		spellchecker/macspellchecker.mm
		)
elseif(WIN32)
	list(APPEND PLAIN_HEADERS
		# spellchecker
		spellchecker/hunspellchecker.h
		)

	list(APPEND PLAIN_SOURCES
		#idle
		idle/idle_win.cpp

		# systemwatch
		systemwatch/systemwatch_win.cpp

		# globalshortcut
		globalshortcut/globalshortcutmanager_win.cpp

		# spellchecker
		spellchecker/hunspellchecker.cpp
		)
else()
	list(APPEND HEADERS
		# systemwatch
		systemwatch/systemwatch_unix.h
		)

	list(APPEND PLAIN_SOURCES
		#idle
		idle/idle_x11.cpp

		# systemwatch
		systemwatch/systemwatch_unix.cpp


		# globalshortcut
		globalshortcut/globalshortcutmanager_x11.cpp
		)
endif()

# spellchecker
if(USE_ENCHANT)
	add_definitions(-DHAVE_ENCHANT)

	include_directories(
		${Enchant_INCLUDE_DIR}
		)

	list(APPEND EXTRA_LDFLAGS
		${Enchant_LIBRARY}
		)

	list(APPEND PLAIN_HEADERS
		spellchecker/enchantchecker.h
		)

	list(APPEND PLAIN_SOURCES
		spellchecker/enchantchecker.cpp
		)
elseif(USE_HUNSPELL)
	add_definitions(-DHAVE_HUNSPELL)

	include_directories(
		${HUNSPELL_INCLUDE_DIR}
		)

	list(APPEND EXTRA_LDFLAGS
		${HUNSPELL_LIBRARY}
		)

	list(APPEND PLAIN_HEADERS
		spellchecker/hunspellchecker.h
		)

	list(APPEND PLAIN_SOURCES
		spellchecker/hunspellchecker.cpp
		)

endif()

qt4_wrap_ui(UI_FORMS ${FORMS})
qt_wrap_cpp(MOC_SOURCES ${SOURCES} ${HEADERS})
add_library(libpsi_tools STATIC  ${SOURCES} ${HEADERS} ${MOC_SOURCES} ${UI_FORMS} ${PLAIN_SOURCES} ${PLAIN_HEADERS})
target_link_libraries(libpsi_tools ${QT_LIBRARIES} tools ${EXTRA_LDFLAGS})
target_include_directories(libpsi_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})

add_subdirectory(zip)
