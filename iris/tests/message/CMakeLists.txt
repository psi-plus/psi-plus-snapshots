find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Xml)

add_executable(message_bookmarkmanager_test bookmarkmanager.cpp)
target_link_libraries(message_bookmarkmanager_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_bookmarkmanager COMMAND message_bookmarkmanager_test)
set_tests_properties(message_bookmarkmanager PROPERTIES TIMEOUT 10)

add_executable(message_bookmarkmanagerflow_test bookmarkmanagerflow.cpp)
target_link_libraries(message_bookmarkmanagerflow_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_bookmarkmanagerflow COMMAND message_bookmarkmanagerflow_test)
set_tests_properties(message_bookmarkmanagerflow PROPERTIES TIMEOUT 10)

add_executable(message_pubsubreplica_test pubsubreplica.cpp)
target_link_libraries(message_pubsubreplica_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core)
add_test(NAME message_pubsubreplica COMMAND message_pubsubreplica_test)
set_tests_properties(message_pubsubreplica PROPERTIES TIMEOUT 10)

add_executable(message_blocking_test blocking.cpp)
target_link_libraries(message_blocking_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_blocking COMMAND message_blocking_test)
set_tests_properties(message_blocking PROPERTIES TIMEOUT 10)

add_executable(message_nativebookmarks_test nativebookmarks.cpp)
target_link_libraries(message_nativebookmarks_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_nativebookmarks COMMAND message_nativebookmarks_test)
set_tests_properties(message_nativebookmarks PROPERTIES TIMEOUT 10)

add_executable(message_displayedmarkers_test displayedmarkers.cpp)
target_link_libraries(message_displayedmarkers_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_displayedmarkers COMMAND message_displayedmarkers_test)
set_tests_properties(message_displayedmarkers PROPERTIES TIMEOUT 10)

add_executable(message_occupantids_test occupantids.cpp)
target_link_libraries(message_occupantids_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_occupantids COMMAND message_occupantids_test)
set_tests_properties(message_occupantids PROPERTIES TIMEOUT 10)

add_executable(message_stanzaids_test stanzaids.cpp)
target_link_libraries(message_stanzaids_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_stanzaids COMMAND message_stanzaids_test)
set_tests_properties(message_stanzaids PROPERTIES TIMEOUT 10)

add_executable(message_delay_test delay.cpp)
target_link_libraries(message_delay_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_delay COMMAND message_delay_test)
set_tests_properties(message_delay PROPERTIES TIMEOUT 10)

add_executable(message_replies_test replies.cpp)
target_link_libraries(message_replies_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_replies COMMAND message_replies_test)
set_tests_properties(message_replies PROPERTIES TIMEOUT 10)

add_executable(message_mds_test mds.cpp)
target_link_libraries(message_mds_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_mds COMMAND message_mds_test)
set_tests_properties(message_mds PROPERTIES TIMEOUT 10)

add_executable(message_moderation_test moderation.cpp)
target_link_libraries(message_moderation_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_moderation COMMAND message_moderation_test)
set_tests_properties(message_moderation PROPERTIES TIMEOUT 10)

add_executable(message_moderation_metadata_test moderationmetadata.cpp)
target_link_libraries(message_moderation_metadata_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_moderation_metadata COMMAND message_moderation_metadata_test)
set_tests_properties(message_moderation_metadata PROPERTIES TIMEOUT 10)

add_executable(message_simsfallback_test simsfallback.cpp)
target_link_libraries(message_simsfallback_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_simsfallback COMMAND message_simsfallback_test)
set_tests_properties(message_simsfallback PROPERTIES TIMEOUT 10)

add_executable(message_vcard4_test vcard4.cpp)
target_link_libraries(message_vcard4_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_vcard4 COMMAND message_vcard4_test)
set_tests_properties(message_vcard4 PROPERTIES TIMEOUT 10)

add_executable(message_styling_test styling.cpp)
target_link_libraries(message_styling_test PRIVATE iris Qt${QT_DEFAULT_MAJOR_VERSION}::Core
    Qt${QT_DEFAULT_MAJOR_VERSION}::Xml)
add_test(NAME message_styling COMMAND message_styling_test)
set_tests_properties(message_styling PROPERTIES TIMEOUT 10)
