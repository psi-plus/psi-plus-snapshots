set( CMAKE_MODULE_PATH
	"${CMAKE_MODULE_PATH}"
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/cmake/modules" )
set(CMAKE_AUTOMOC OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32)
	set(D "d")
	add_definitions(-DALLOW_QT_PLUGINS_DIR)
	if(LINUX)
		set(EXTRA_FLAG "-ggdb")
	endif()
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 ${EXTRA_FLAG}")
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 ${EXTRA_FLAG}")
endif()

set( LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )

if(USE_QT5)
	find_package(Qt5 REQUIRED Widgets Core Gui Xml Network Concurrent Multimedia Svg LinguistTools)
	if(ENABLE_WEBKIT)
		find_package(Qt5WebKit REQUIRED)
		find_package(Qt5WebKitWidgets REQUIRED)
		set(WEBKIT_LIBS Qt5::WebKit Qt5::WebKitWidgets)
	endif()
	if(ENABLE_SQL)
		find_package(Qt5Sql REQUIRED)
		set(SQL_LIBS Qt5::Sql)
	endif()

	if(LINUX)
		find_package(Qt5DBus REQUIRED)
		find_package(Qt5X11Extras REQUIRED)
		find_package(XCB REQUIRED)
		set(EXTRA_LIBS
			${XCB_LIBRARY}
			)
		include_directories(${LIBXCB_INCLUDE_DIR})
		add_definitions(${LIBXCB_DEFINITIONS})
	endif()

	add_definitions(-DHAVE_QT5 ${LIBXCB_DEFINITIONS})

	macro(QT4_CREATE_TRANSLATION)
		QT5_CREATE_TRANSLATION(${ARGN})
	endmacro()

	macro(QT4_ADD_TRANSLATION)
		qt5_add_translation(${ARGN})
	endmacro()

	macro(qt4_add_resources)
		qt5_add_resources(${ARGN})
	endmacro()

	macro(qt4_wrap_ui)
		qt5_wrap_ui(${ARGN})
	endmacro()

	macro(qt4_generate_moc)
		qt5_generate_moc(${ARGN})
	endmacro()

	set(QT_LUPDATE_EXECUTABLE ${Qt5_LUPDATE_EXECUTABLE})
	set(QT_LIBRARIES Qt5::Widgets Qt5::Core Qt5::Network Qt5::Xml ${WEBKIT_LIBS} Qt5::Concurrent Qt5::Multimedia Qt5::Svg ${SQL_LIBS})
	find_package( Qca-qt5 REQUIRED )
	set(QCA_INCLUDES ${Qca-qt5_INCLUDE_DIR})
	set(qca_LIB ${Qca-qt5_LIBRARY})
	if(LINUX)
		list(APPEND QT_LIBRARIES Qt5::DBus Qt5::X11Extras ${LIBXCB_LIBRARIES})
	endif()
else()
	if(LINUX)
		set(QT_EXTRA_MODULES QtDBus)
	endif()
	find_package(Qt4 REQUIRED QtCore QtGui QtNetwork QtXml QtSvg ${QT_EXTRA_MODULES})
	if(ENABLE_WEBKIT)
		find_package(Qt4 REQUIRED QtWebKit)
	endif()
	if(ENABLE_SQL)
		find_package(Qt4 REQUIRED QtSql)
	endif()

	unset(QT_EXTRA_MODULES)

	include(${QT_USE_FILE})
	find_package( Qca REQUIRED )
	set(QCA_INCLUDES ${Qca_INCLUDE_DIR})
	set(qca_LIB ${Qca_LIBRARY})
	if(ENABLE_SQL)
		find_package(QJSON REQUIRED)
		set(EXTRA_LIBS
			${EXTRA_LIBS}
			${QJSON_LIBRARY}
			)
		include_directories(${QJSON_INCLUDE_DIR})
		if( WIN32 )
			include_directories( ${QJSON_INCLUDE_DIR}/../ )
		endif()
	endif()
endif()

if(LINUX)
	find_package(X11 REQUIRED)
	include_directories(${X11_INCLUDE_DIR})
endif()

if(USE_ENCHANT)
	find_package(Enchant REQUIRED)
	set(EXTRA_LIBS
		${EXTRA_LIBS}
		${Enchant_LIBRARY}
		)
	include_directories(${Enchant_INCLUDE_DIR})
endif()
if(USE_HUNSPELL)
	find_package(Hunspell REQUIRED)
	set(EXTRA_LIBS
		${EXTRA_LIBS}
		${HUNSPELL_LIBRARY}
		)
	include_directories(${HUNSPELL_INCLUDE_DIR})
	if( WIN32 )
		include_directories( ${HUNSPELL_INCLUDE_DIR}/../ )
	endif()
endif()

macro(qt_wrap_cpp)
	set(_SOURCES ${ARGN})
	list(REMOVE_AT _SOURCES 0)
	foreach(SOURCE ${_SOURCES})
		if(SOURCE MATCHES "^(.*/|)([^/.]+)\\.h$")
			set(DEST "moc_${CMAKE_MATCH_2}.cpp")
			qt4_generate_moc(${SOURCE} ${DEST} ${PROJECT_NAME})
			list(APPEND ${ARGV0} ${DEST})
		elseif(SOURCE MATCHES "^(.*/|)([^/.]+)\\.cpp$")
			set(DEST "${CMAKE_MATCH_2}.moc")
			qt4_generate_moc(${SOURCE} ${DEST} TARGET ${PROJECT_NAME})
			list(APPEND ${ARGV0} ${DEST})
		else()
			message(WARNING "Wrong source ${SOURCE}")
		endif()
	endforeach()
	unset(_SOURCES)
endmacro()

include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}
	${QCA_INCLUDES}
	)

if( BUNDLED_IRIS )
	include_directories(
		../iris/src
		../iris/include
		../iris/include/iris
		)
else()
	include_directories(${Iris_INCLUDE_DIR})
endif()

include(getdate.cmake)
if(ENABLE_WEBKIT)
	set(PSI_VER_SUFFIX -webkit)
endif()
add_definitions(-DPSI_VERSION="${PSI_PLUS_VERSION}${PSI_VER_SUFFIX} \(${PSI_COMPILATION_DATE}\)")

if(LINUX)
	if(LIB_INSTALL_DIR)
		set(PSI_LIBDIR "${LIB_INSTALL_DIR}/${PROJECT_NAME}")
	else()
		set(PSI_LIBDIR "${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}/${PROJECT_NAME}")
	endif()

	if(SHARE_INSTALL_PREFIX)
		set(PSI_DATADIR "${SHARE_INSTALL_PREFIX}/${PROJECT_NAME}")
	else()
		set(PSI_DATADIR "${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}")
	endif()
endif()

configure_file(config.h.in config.h @ONLY)

add_definitions(
	-DQT_STATICPLUGIN
	-DHAVE_CONFIG
	)

if(WIN32)
	add_definitions(
		-DUNICODE
		-D_UNICODE
		)
endif()

include(src.cmake)

# Only headers or very small sources
include(irisprotocol/irisprotocol.cmake)
include(protocol/protocol.cmake)
include(plugins/plugins.cmake)

add_subdirectory(options)
add_subdirectory(tabs)
add_subdirectory(privacy)
add_subdirectory(Certificates)
add_subdirectory(avcall)
add_subdirectory(psimedia)
add_subdirectory(contactmanager)
add_subdirectory(tools)
add_subdirectory(libpsi/dialogs)
add_subdirectory(libpsi/tools)
add_subdirectory(widgets)
add_subdirectory(sxe)
add_subdirectory(whiteboarding)

if(EXISTS "${PROJECT_SOURCE_DIR}/translations")

	set(TRANSLATIONS
		../translations/psi_be.ts
		../translations/psi_bg.ts
		../translations/psi_ca.ts
		../translations/psi_cs.ts
		../translations/psi_de.ts
		../translations/psi_en.ts
		../translations/psi_eo.ts
		../translations/psi_es.ts
		../translations/psi_et.ts
		../translations/psi_fa.ts
		../translations/psi_fi.ts
		../translations/psi_fr.ts
		../translations/psi_he.ts
		../translations/psi_hu.ts
		../translations/psi_it.ts
		../translations/psi_ja.ts
		../translations/psi_kk.ts
		../translations/psi_mk.ts
		../translations/psi_nl.ts
		../translations/psi_pl.ts
		../translations/psi_pt_BR.ts
		../translations/psi_pt.ts
		../translations/psi_ru.ts
		../translations/psi_sk.ts
		../translations/psi_sl.ts
		../translations/psi_sr@latin.ts
		../translations/psi_sv.ts
		../translations/psi_sw.ts
		../translations/psi_uk.ts
		../translations/psi_ur_PK.ts
		../translations/psi_vi.ts
		../translations/psi_zh_CN.ts
		../translations/psi_zh_TW.ts
		)

	set_property(SOURCE ${TRANSLATIONS} PROPERTY OUTPUT_LOCATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations)

	add_custom_target(lupdate)
	foreach(_LANG ${TRANSLATIONS})
		get_filename_component(_SHORT_LANG ${_LANG} NAME_WE)
		string(REPLACE "@" "_" _SHORT_LANG ${_SHORT_LANG})
		add_custom_target(${_SHORT_LANG}
			COMMAND ${QT_LUPDATE_EXECUTABLE} "-no-obsolete" "-recursive" "." "../iris" "-ts" ${_LANG}
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
		add_dependencies(lupdate ${_SHORT_LANG})
	endforeach()

	QT4_ADD_TRANSLATION(QM ${TRANSLATIONS})
endif()

set(RESOURCES ../psi.qrc ../iconsets.qrc)
qt4_add_resources(QRC_SOURCES ${RESOURCES})

qt4_wrap_ui(UI_FORMS ${FORMS})
add_custom_target(build_ui_files DEPENDS "${UI_FORMS}" SOURCES "${FORMS}")

qt_wrap_cpp(MOC_SOURCES ${SOURCES} ${HEADERS})

if(WIN32)
	# resource compilation for MinGW
	if(NOT MSVC)
		if(BUILD_ARCH STREQUAL "i386")
			add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o
				COMMAND ${CMAKE_RC_COMPILER} -I${CMAKE_CURRENT_SOURCE_DIR} -i${CMAKE_SOURCE_DIR}/win32/psi_win32.rc
				-o ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o)
		else()
			add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o
				COMMAND ${CMAKE_RC_COMPILER} -I${CMAKE_CURRENT_SOURCE_DIR} -i${CMAKE_SOURCE_DIR}/win32/psi_win64.rc
				-o ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o)
		endif()
	else()
		if(BUILD_ARCH STREQUAL "win32")
			add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o
				COMMAND ${CMAKE_RC_COMPILER} /fo ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o ${CMAKE_SOURCE_DIR}/win32/psi_win32.rc)
		else()
			add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o
				COMMAND ${CMAKE_RC_COMPILER} /fo ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o ${CMAKE_SOURCE_DIR}/win32/psi_win64.rc)
		endif()
	endif()
	set(RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/psi_win32.o)
endif()

add_executable(${PROJECT_NAME}
	WIN32
	MACOSX_BUNDLE
	${SOURCES}
	${HEADERS}
	${MOC_SOURCES}
	${UI_FORMS}
	${PLAIN_SOURCES}
	${PLAIN_HEADERS}
	${QRC_SOURCES}
	${QM}
	${RC_FILE}
	)

add_dependencies(${PROJECT_NAME} iris)
add_dependencies(${PROJECT_NAME} build_ui_files)

target_link_libraries(${PROJECT_NAME}
	${QT_LIBRARIES}
	${EXTRA_LDFLAGS}
	${X11_LIBRARIES}
	${EXTRA_LIBS}
	iris
	${qca_LIB}
	)
target_link_libraries(${PROJECT_NAME}
	options
	Certificates
	psimedia
	avcall
	contactmanager
	whiteboarding
	sxe
	tools
	libpsi_dialogs
	libpsi_tools
	widgets
	privacy
	tabs
	)

set(OTHER_FILES
	../certs
	../iconsets
	../sound
	)
if(LINUX)
	set(SHARE_SUFF "share/psi-plus")
	install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
	install(FILES ../client_icons.txt DESTINATION ${CMAKE_INSTALL_PREFIX}/${SHARE_SUFF})
	install(FILES ../psi.desktop DESTINATION ${CMAKE_INSTALL_PREFIX}/share/applications RENAME psi-plus.desktop)
	install(DIRECTORY ${OTHER_FILES} DESTINATION ${CMAKE_INSTALL_PREFIX}/${SHARE_SUFF})
	install(FILES ../iconsets/system/default/psiplus/logo_128.png DESTINATION ${CMAKE_INSTALL_PREFIX}/share/pixmaps RENAME psi-plus.png)
	if(ENABLE_WEBKIT)
		install(DIRECTORY ../themes DESTINATION ${CMAKE_INSTALL_PREFIX}/${SHARE_SUFF})
	endif()
endif()

if(WIN32)
	install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX})
	install(FILES ../client_icons.txt DESTINATION ${CMAKE_INSTALL_PREFIX})
	install(DIRECTORY ${OTHER_FILES} DESTINATION ${CMAKE_INSTALL_PREFIX})
	if(ENABLE_WEBKIT)
		install(FILES ../themes/chatview/util.js DESTINATION ${CMAKE_INSTALL_PREFIX}/themes/chatview)
		install(FILES ../themes/chatview/psi/adapter.js DESTINATION ${CMAKE_INSTALL_PREFIX}/themes/chatview/psi)
		install(FILES ../themes/chatview/adium/adapter.js DESTINATION ${CMAKE_INSTALL_PREFIX}/themes/chatview/adium)
	endif()
endif()

if(ENABLE_PLUGINS)
	add_subdirectory(plugins)
endif()

if(WIN32)
	if(PRODUCTION)
		# Get Qt installation path
		string(REGEX REPLACE "([^ ]+)[/\\].*" "\\1" QT_BIN_DIR_TMP "${QT_MOC_EXECUTABLE}")
		string(REGEX REPLACE "\\\\" "/" QT_BIN_DIR "${QT_BIN_DIR_TMP}")
		unset(QT_BIN_DIR_TMP)

		get_filename_component(QT_DIR ${QT_BIN_DIR} DIRECTORY)
		set(QT_PLUGINS_DIR ${QT_DIR}/plugins)
		set(QT_TRANSLATIONS_DIR ${QT_DIR}/translations)

		if(USE_QT5)
			# required libraries
			copy("${QT_BIN_DIR}/icudt54.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/icuin54.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/icuuc54.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

			# Qt5 libraries
			copy("${QT_BIN_DIR}/Qt5Core${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Gui${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Widgets${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Svg${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Network${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Svg${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Script${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Xml${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5XmlPatterns${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Sql${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5WebKit${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5WebKitWidgets${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Qml${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Quick${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Positioning${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5WebChannel${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Multimedia${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5MultimediaWidgets${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Concurrent${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5Sensors${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5OpenGL${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/Qt5PrintSupport${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

			#audio
			copy("${QT_PLUGINS_DIR}/audio/qtaudio_windows${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/audio/" prepare-bin-libs)
			# platforms
			copy("${QT_PLUGINS_DIR}/platforms/qminimal${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/platforms/qoffscreen${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/platforms/qwindows${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/platforms/" prepare-bin-libs)

			# bearer
			copy("${QT_PLUGINS_DIR}/bearer/qgenericbearer${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/bearer/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/bearer/qnativewifibearer${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/bearer/" prepare-bin-libs)

			# imageformats
			copy("${QT_PLUGINS_DIR}/imageformats/qdds${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qgif${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qicns${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qico${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qjp2${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qjpeg${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qmng${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qsvg${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qtga${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qtiff${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qwbmp${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qwebp${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)

			# sqldrivers
			copy("${QT_PLUGINS_DIR}/sqldrivers/qsqlite${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sqldrivers/" prepare-bin-libs)

			# Qt translations
			file(GLOB QT_TRANSLATIONS "${QT_TRANSLATIONS_DIR}/qt_*.qm")
			foreach(FILE ${QT_TRANSLATIONS})
				if(NOT FILE MATCHES "_help_")
					copy(${FILE} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations/" prepare-bin-libs)
				endif()
			endforeach()

			file(GLOB QT_TRANSLATIONS "${QT_TRANSLATIONS_DIR}/qtbase_*.qm")
			foreach(FILE ${QT_TRANSLATIONS})
				copy(${FILE} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations/" prepare-bin-libs)
			endforeach()

			copy("${QCA_DIR}/bin/libqca-qt5${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		else()
			# Qt4 libs
			copy("${QT_BIN_DIR}/QtCore${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/QtGui${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/QtNetwork${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/QtSvg${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/QtXml${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/QtSql${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QT_BIN_DIR}/QtWebKit${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

			# bearer
			copy("${QT_PLUGINS_DIR}/bearer/qgenericbearer${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/bearer/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/bearer/qnativewifibearer${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/bearer/" prepare-bin-libs)

			# plugins
			copy("${QT_PLUGINS_DIR}/imageformats/qgif${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qico${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qjpeg${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qmng${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qtga4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qtiff${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)
			copy("${QT_PLUGINS_DIR}/imageformats/qsvg${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/imageformats/" prepare-bin-libs)

			# sqldrivers
			copy("${QT_PLUGINS_DIR}/sqldrivers/qsqlite${D}4.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sqldrivers/" prepare-bin-libs)

			# Qt translations
			file(GLOB QT_TRANSLATIONS "${QT_TRANSLATIONS_DIR}/qt_*.qm")
			foreach(FILE ${QT_TRANSLATIONS})
				if(NOT FILE MATCHES "_help_")
					copy(${FILE} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations/" prepare-bin-libs)
				endif()
			endforeach()

			copy("${QCA_DIR}/bin/libqca${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy("${QJSON_ROOT}/bin/libqjson${D}.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		endif()

		copy("${QT_BIN_DIR}/libwinpthread-1.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy("${QT_BIN_DIR}/libgcc_s_sjlj-1.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy("${QT_BIN_DIR}/libstdc++-6.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

		# psimedia
		find_program(PSIMEDIA_PATH libgstprovider${D}.dll PATHS ${PSIMEDIA_DIR}/plugins )
		copy(${PSIMEDIA_DIR} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

		# psimedia deps
		find_program(PSIMEDIA_DEPS_PATH libgstvideo-0.10-0.dll PATHS ${GST_SDK}/bin )
		get_filename_component(PSIMEDIA_DEPS_DIR ${PSIMEDIA_DEPS_PATH} DIRECTORY)
		copy(${PSIMEDIA_DEPS_DIR}/libjpeg-9.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgettextlib-0-19-6.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libogg-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libtheoradec-1.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgettextpo-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/liborc-0.4-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libtheoraenc-1.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libasprintf-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgettextsrc-0-19-6.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/liborc-test-0.4-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libvorbis-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libcharset-1.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgio-2.0-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libspeex-1.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libvorbisenc-2.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libglib-2.0-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgthread-2.0-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libspeexdsp-1.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libvorbisfile-3.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libffi-6.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgmodule-2.0-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgobject-2.0-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libintl-8.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libtheora-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstapp-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstaudio-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstbase-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstcdda-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstcontroller-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstdataprotocol-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstfft-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstinterfaces-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstnet-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstnetbuffer-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstpbutils-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstreamer-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstriff-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstrtp-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstrtsp-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstsdp-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgsttag-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${PSIMEDIA_DEPS_DIR}/libgstvideo-0.10-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

		# streamer plugins
		set(GSTREAMER_PLUGINS_DIR "${PSIMEDIA_DIR}/gstreamer-0.10")
		file(GLOB GSTREAMER_PLUGINS "${GSTREAMER_PLUGINS_DIR}/*.dll")
		foreach(FILE ${GSTREAMER_PLUGINS})
			copy(${FILE} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/gstreamer-0.10/" prepare-bin-libs)
		endforeach()

		# other libs and executables
		copy(${LIBGCRYPT_ROOT}/bin/libgcrypt-11.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${LIBOTR_ROOT}/bin/libotr.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		#copy(${OTHER_DIR}/gpg.exe "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${ZLIB_ROOT}/bin/zlib1.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${LIBGPGERROR_ROOT}/bin/libgpg-error-0.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		if(SEPARATE_QJDNS)
			copy(${QJDNS_DIR}/libqjdns${D}.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
			copy(${QJDNS_DIR}/libjdns${D}.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		endif()
		copy(${IDN_ROOT}/bin/libidn-11.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		#copy(${OTHER_DIR}/libiconv-2.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		#copy(${OTHER_DIR}/libintl-8.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${HUNSPELL_ROOT}/bin/libhunspell.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		if(USE_QT5)
			set(SDK_PREFIX "Qt5/")
		endif()
		copy(${SDK_PATH}/${SDK_PREFIX}openssl/bin/libeay32.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)
		copy(${SDK_PATH}/${SDK_PREFIX}openssl/bin/ssleay32.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" prepare-bin-libs)

		# qca plugins
		get_filename_component(QCA_PLUGINS_DIR ${QCA_DIR} DIRECTORY)
		set(QCA_PLUGINS_DIR "${QCA_PLUGINS_DIR}/lib/qca/crypto")
		copy(${QCA_PLUGINS_DIR}/libqca-ossl${D}.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/crypto/" prepare-bin-libs)
		copy(${QCA_PLUGINS_DIR}/libqca-gnupg${D}.dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/crypto/" prepare-bin-libs)
	endif()
endif()
