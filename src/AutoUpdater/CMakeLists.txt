unset(MOC_SOURCES)
unset(HEADERS)
unset(PLAIN_HEADERS)
unset(FORMS)
unset(SOURCES)
unset(PLAIN_SOURCES)
unset(UI_FORMS)

include_directories(
	../
)

list(APPEND HEADERS
	AutoUpdater.h
)

list(APPEND PLAIN_SOURCES
	AutoUpdater.cpp
)

if(APPLE)
	list(APPEND HEADERS
		SparkleAutoUpdater.h
	)
	list(APPEND SOURCES
		SparkleAutoUpdater.mm
	)
	#set(EXTRA_LIB
	#	Sparkle
	#)
endif()

if(WIN32 AND IS_PSIPLUS)
	add_definitions(-DUSE_QDCHECKER)
	list(APPEND HEADERS
		QDChecker.h
	)
	list(APPEND PLAIN_SOURCES
		QDChecker.cpp
	)
	list(APPEND FORMS
		QDChangeLog.ui
	)
endif()

qt4_wrap_ui(UI_FORMS ${FORMS})
qt_wrap_cpp(MOC_SOURCES ${HEADERS})
add_library(AutoUpdater STATIC ${HEADERS} ${MOC_SOURCES} ${UI_FORMS} ${PLAIN_SOURCES})
target_link_libraries(AutoUpdater ${QT_LIBRARIES})
target_include_directories(AutoUpdater PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
